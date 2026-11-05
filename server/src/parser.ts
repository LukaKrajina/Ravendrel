import { Token, TokenType, Lexer } from './lexer';
import { expandMorphTokens } from './morph';
import {
    Program,
    Statement,
    Item,
    VariableDeclaration,
    VariableDeclarationList,
    Expression,
    WhileStatement,
    ForStatement,
    IfStatement,
    RouteStatement,
    RouteCase,
    SpinStatement,
    UnsafeBlock,
    BreakStatement,
    ContinueStatement,
    ReturnStatement,
    AssignmentStatement,
    ExpressionStatement,
    FunctionCall,
    NumberLiteral,
    NullLiteral,
    Dereference,
    AddressOf,
    NativeExpression,
    FuseExpression,
    FuseArm,
    Identifier,
    FunctionDeclaration,
    FunctionAttribute,
    LayerTag,
    QuantumAttrs,
    PhysicalAttrs,
    BinaryExpression,
    LogicalExpression,
    UnaryExpression,
    ResultExpr,
    FunctionExpression,
    MemberExpression,
    StringLiteral,
    NewExpression,
    IndexExpression,
    ModuleDecl,
    UseDecl,
    FormDecl,
    FlavorDecl,
    FlavorMember,
    GlobalVarDecl,
    ImplDecl,
    TraitDecl,
    TemplateDecl,
    RankBlock,
    FieldDecl,
    FnDecl,
    Param,
    InheritClause,
    ImportDecl,
    RequiresDecl,
    ExternDecl,
    SpawnStatement,
    EntangleStatement,
    AsmStatement,
    ArrayLiteral,
    ShuttleStrategyValue
} from './ast';

/**
 * `@layer(...)` 的标量键全集（值形如 `key=N`）。
 *
 * 不在此表中的键会被 parseLayerTag 记入 `tag.unknownKeys` 并报 E-TOP008，
 * 而不是像原先那样被静默丢弃。
 */
export const LAYER_TAG_SCALAR_KEYS: ReadonlySet<string> = new Set([
    'time', 'thread', 'cost', 'deadline', 'dual',
    'shuttle_lambda', 'shuttle_phase', 'shuttle_rounds', 'shuttle_strategy',
    'track_mode', 'track_slots',
]);

/**
 * `@layer(...)` 的列表型键（值形如 `key=(...)`）。与标量键一同构成键全集。
 * 单独列出是因为它们的取值是括号列表（`coord` / `shuttle` 为数值列表、`track` 为标识符列表）。
 */
export const LAYER_TAG_LIST_KEYS: ReadonlySet<string> = new Set(['coord', 'shuttle', 'track']);

/** 量子门属性名（@[gate] / @[undo] / @[steer] / @[unitary] / @[measure]） */
const QUANTUM_ATTR_NAMES = new Set(['gate', 'undo', 'steer', 'unitary', 'measure']);

/**
 * 从函数属性里分离量子门属性，返回结构化 QuantumAttrs（无则 undefined）。
 * 分离后这些量子属性不再保留在系统属性列表里（它们不属于 LLVM 函数属性）。
 */
function extractQuantumAttrs(attributes: FunctionAttribute[]): { quantum?: QuantumAttrs; system: FunctionAttribute[] } {
    let quantum: QuantumAttrs | undefined;
    const system: FunctionAttribute[] = [];
    for (const a of attributes) {
        if (QUANTUM_ATTR_NAMES.has(a.name)) {
            if (!quantum) quantum = { isGate: false, undo: false, steer: false, unitary: false, measure: false };
            if (a.name === 'gate') quantum.isGate = true;
            else if (a.name === 'undo') quantum.undo = true;
            else if (a.name === 'steer') quantum.steer = true;
            else if (a.name === 'unitary') quantum.unitary = true;
            else if (a.name === 'measure') quantum.measure = true;
        } else {
            system.push(a);
        }
    }
    return { quantum, system };
}

/** 量子物理特性属性名（@[coherence] / @[noise] / @[basis] / @[decoherence_free] / @[error_correction]） */
const PHYSICAL_ATTR_NAMES = new Set(['coherence', 'noise', 'basis', 'decoherence_free', 'error_correction']);

/**
 * 从函数属性里分离量子物理特性，返回结构化 PhysicalAttrs（无则 undefined）。
 * 物理特性为 QVM/QM 模拟提供约束元数据，不属于 LLVM 函数属性。
 */
function extractPhysicalAttrs(attributes: FunctionAttribute[]): { physical?: PhysicalAttrs; system: FunctionAttribute[] } {
    let physical: PhysicalAttrs | undefined;
    const system: FunctionAttribute[] = [];
    for (const a of attributes) {
        if (PHYSICAL_ATTR_NAMES.has(a.name)) {
            if (!physical) physical = {};
            if (a.name === 'coherence') {
                const nums = a.nums ?? [];
                physical.coherence = { t1: nums[0] ?? 0, t2: nums[1] ?? 0 };
            } else if (a.name === 'noise') {
                physical.noise = a.value ?? '';
            } else if (a.name === 'basis') {
                const v = (a.value ?? 'Z').toUpperCase();
                if (v === 'X' || v === 'Y' || v === 'Z') physical.basis = v;
            } else if (a.name === 'decoherence_free') {
                physical.decoherenceFree = true;
            } else if (a.name === 'error_correction') {
                physical.errorCorrection = a.value ?? '';
            }
        } else {
            system.push(a);
        }
    }
    return { physical, system };
}

const ALLOWED_TYPES = [
    'let', 'auto', 'int', 'int8', 'int16', 'int32', 'int64',
    'uint8', 'uint16', 'uint32', 'uint64',
    'float', 'double', 'complex64', 'complex128', 'string', 'char', 'Qubit', 'QObject', 'QModel',
    'DiracState', 'BellState', 'QuantumRegister',
    'cap',
    'lattice',
    'arr',
    'fn',
    'fixed'
];

const BUILTIN_FUNCTIONS = [
    'alloc', 'measure', 'basis_state', 'encode_text', 'encode_image', 'encode_amplitudes', 'qobj_num_qubits', 'qlm_invoke',
    'qlm_load', 'rvdl_encode_string', 'qlm_forward', 'rvdl_decode_string',
    'mind_read', 'mind_train', 'mind_feedback',
    'veda_qlm_train',
    'surrogate', 'tanh_quantize', 'lif_step',
    'mellowmax2', 'logsumexp2', 'boltzmann2',
    'tnorm_luk', 'tnorm_prod', 'tnorm_godel',
    'polymer_weight', 'polymer_mix_bound',
    'rvdl_sys_call', 'rvdl_sys_calld', 'rvdl_sys_log', 'rvdl_sys_logi',
    'rvdl_sys_callp', 'rvdl_gc_free',
    'rvdl_qms_gap', 'rvdl_mix_bound', 'rvdl_qms_conc',
    'qchain_wallet', 'qchain_mint', 'qchain_transfer', 'qchain_balance',
    'qchain_mine', 'qchain_height', 'qchain_verify',
    'qchain_qkd', 'qchain_qdba', 'qchain_coin_mint', 'qchain_coin_verify',
    'qchain_sha3', 'qchain_hmac', 'qchain_hash_unicode',
    'qchain_sign', 'qchain_sign_verify', 'qchain_sign_pubkey',
    'qchain_mlkem_encaps', 'qchain_mlkem_decaps',
    'qchain_causal_verify', 'qchain_cipher_encrypt', 'qchain_cipher_decrypt',
    'sync_load', 'sync_store', 'sync_add', 'sync_cas',
    'sync_lock', 'sync_unlock',
    'native', 'outb', 'inb', 'outw', 'inw', 'outl', 'inl',
    'rvdl_gc_alloc', 'addr',
    'read_cr0', 'read_cr2', 'read_cr3', 'read_cr4',
    'write_cr0', 'write_cr3', 'invlpg',
    'rdmsr', 'wrmsr', 'cpuid', 'rdtsc', 'read_rflags', 'xgetbv',
    'volatile_load', 'volatile_store',
    'complex', 'real', 'imag', 'cabs', 'conj'
];

export class Parser {
    private lexer: Lexer;
    private currentToken: Token;
    private lookahead: Token | null = null;

    constructor(lexer: Lexer) {
        this.lexer = lexer;
        this.currentToken = this.lexer.getNextToken();
    }

    private advance(): Token {
        const token = this.currentToken;
        if (this.lookahead) {
            this.currentToken = this.lookahead;
            this.lookahead = null;
        } else {
            this.currentToken = this.lexer.getNextToken();
        }
        return token;
    }

    private peek(): Token {
        if (!this.lookahead) {
            this.lookahead = this.lexer.getNextToken();
        }
        return this.lookahead;
    }

    private eat(type: TokenType): Token {
        const token = this.currentToken;
        if (this.currentToken.type === type) {
            this.advance();
            return token;
        } else {
            throw new Error(`Parser Error: Expected ${type}, found ${this.currentToken.type} ('${this.currentToken.value}') at line ${this.currentToken.line}, col ${this.currentToken.column}`);
        }
    }

    private isKeyword(value: string): boolean {
        return this.currentToken.type === TokenType.Keyword && this.currentToken.value === value;
    }

    private eatKeyword(value: string): Token {
        if (!this.isKeyword(value)) {
            throw new Error(`Parser Error: Expected keyword '${value}', found '${this.currentToken.value}' at line ${this.currentToken.line}, col ${this.currentToken.column}`);
        }
        return this.advance();
    }

    private eatIdentifierOrKeyword(): Token {
        const token = this.currentToken;
        if (token.type === TokenType.Identifier || token.type === TokenType.Keyword) {
            this.advance();
            return token;
        }
        throw new Error(`Parser Error: Expected identifier, found ${token.type} ('${token.value}') at line ${token.line}, col ${token.column}`);
    }

    public parse(): Program {
        // morph 态射宏展开（词法层）：先把 token 流整体取出交给 morph.ts 做
        // 收集 + 不动点展开，再用 Lexer.fromTokens 切回解析。无 morph 时原样通过。
        const expanded = expandMorphTokens(this.drainTokens());
        this.lexer = Lexer.fromTokens(expanded);
        this.currentToken = this.lexer.getNextToken();
        this.lookahead = null;

        const startLine = this.currentToken.line;
        const startCol = this.currentToken.column;
        const program: Program = {
            type: 'Program',
            body: [],
            line: startLine,
            column: startCol,
            length: 0
        };
        while (this.currentToken.type !== TokenType.EOF) {
            program.body.push(this.parseTopLevelItem());
        }
        return program;
    }

    /** 取出当前 lexer 中尚未消费的全部非 EOF token（morph 展开前） */
    private drainTokens(): Token[] {
        const tokens: Token[] = [];
        while (this.currentToken.type !== TokenType.EOF) {
            tokens.push(this.advance());
        }
        return tokens;
    }

    /**
     * 函数属性：@[name]、@[name("value")]、@[name(n1, n2, ...)]，连续多个。
     * 用于系统级编程（@[section]/@[naked]/@[inline]）与量子物理特性（@[coherence(100,50)]）。
     */
    private parseAttributes(): FunctionAttribute[] {
        const attrs: FunctionAttribute[] = [];
        // 仅解析 @[...] 属性；遇到 @layer(...) 等其它 @ 标签时停下，交由 parseTopLevelItem 处理。
        while (this.currentToken.type === TokenType.At && this.peek().type === TokenType.OpenBracket) {
            this.eat(TokenType.At);
            this.eat(TokenType.OpenBracket);
            const name = this.eatIdentifierOrKeyword().value;
            let value: string | null = null;
            let nums: number[] | undefined;
            // `as` 断言打断 while 循环对 this.currentToken.type 的流式窄化
            const t = this.currentToken.type as TokenType;
            if (t === TokenType.OpenParen) {
                this.eat(TokenType.OpenParen);
                const first = this.currentToken.type as TokenType;
                if (first === TokenType.String) {
                    value = this.eat(TokenType.String).value;
                } else {
                    // 数值参数列表（如 @[coherence(100, 50)]）
                    nums = [];
                    while ((this.currentToken.type as TokenType) !== TokenType.CloseParen && (this.currentToken.type as TokenType) !== TokenType.EOF) {
                        nums.push(Number(this.eat(TokenType.Number).value));
                        if ((this.currentToken.type as TokenType) === TokenType.Comma) this.eat(TokenType.Comma);
                    }
                }
                this.eat(TokenType.CloseParen);
            }
            this.eat(TokenType.CloseBracket);
            attrs.push({ name, value, nums });
        }
        return attrs;
    }

    /**
     * 运行层标签：
     *   @layer(time=T, thread=W, coord=(x,y,...)[, cost=N][, deadline=D][, dual=1]
     *         [, shuttle=(c,...)[, shuttle_lambda=L][, shuttle_phase=P][, shuttle_rounds=R][, shuttle_strategy=S]]
     *         [, track=(obs,...)[, track_mode=M][, track_slots=K]])
     *
     * 解析后返回 LayerTag；`time` 可省略（子函数运行时继承调用者时钟 + Δt）。
     * 穿梭/追踪键**可选且默认惰性**——缺省时既有语义与诊断逐字节不变。
     *
     * 前置条件：当前 token 为 '@'，peek 为标识符 'layer'。
     *
     * 未知键**不再静默丢弃**：记入 `tag.unknownKeys`，由 semantic.ts 报 E-TOP008。
     * 原有 else-if 链缺 else 分支，拼错的键（如 `thread2=1`）会无声失效——
     * 新机制引入更多键后这会变成真实的用户困惑来源。
     */
    private parseLayerTag(): LayerTag {
        this.eat(TokenType.At);
        this.eatIdentifierOrKeyword();       // 'layer'
        this.eat(TokenType.OpenParen);

        const tag: LayerTag = { thread: 0, coord: [] };
        const unknown: { key: string; line: number; column: number }[] = [];
        while (this.currentToken.type !== TokenType.CloseParen && this.currentToken.type !== TokenType.EOF) {
            const keyTok = this.eatIdentifierOrKeyword();
            const key = keyTok.value;
            this.eat(TokenType.Equals);

            if (key === 'coord') {
                tag.coord = this.parseLayerNumberList();
            } else if (key === 'shuttle') {
                // shuttle=(c₀,…,c_{m−1})：目标 coord，如 shuttle=(1,0) → coord=(1,0)。
                // 多个穿梭群用嵌套括号分隔：shuttle=((1,0),(2,1))。
                const sh = (tag.shuttle = tag.shuttle ?? { targets: [] });
                sh.targets.push(...this.parseLayerCoordGroups());
            } else if (key === 'track') {
                // track=(obs₀,…,obs_{k−1})：标识符列表
                const tr = (tag.track = tag.track ?? { observables: [] });
                this.eat(TokenType.OpenParen);
                while ((this.currentToken.type as TokenType) !== TokenType.CloseParen && (this.currentToken.type as TokenType) !== TokenType.EOF) {
                    tr.observables.push(this.eatIdentifierOrKeyword().value);
                    if ((this.currentToken.type as TokenType) === TokenType.Comma) this.eat(TokenType.Comma);
                }
                this.eat(TokenType.CloseParen);
            } else if (LAYER_TAG_SCALAR_KEYS.has(key)) {
                const v = this.parseLayerScalarValue();
                if (key === 'time') tag.time = v;
                else if (key === 'thread') tag.thread = v;
                else if (key === 'cost') tag.cost = v;
                else if (key === 'deadline') tag.deadline = v;
                else if (key === 'dual') tag.dual = (v !== 0);   // 对偶标记（T-对偶反转空间）
                else if (key === 'shuttle_lambda') (tag.shuttle = tag.shuttle ?? { targets: [] }).lambda = v;
                else if (key === 'shuttle_phase') (tag.shuttle = tag.shuttle ?? { targets: [] }).phase = v;
                else if (key === 'shuttle_rounds') (tag.shuttle = tag.shuttle ?? { targets: [] }).rounds = v;
                else if (key === 'shuttle_strategy') (tag.shuttle = tag.shuttle ?? { targets: [] }).strategy = v as ShuttleStrategyValue;
                else if (key === 'track_mode') (tag.track = tag.track ?? { observables: [] }).mode = v;
                else if (key === 'track_slots') (tag.track = tag.track ?? { observables: [] }).slots = v;
            } else {
                // 未知键：吃掉一个值以保持解析进度，随后报 E-TOP008。
                unknown.push({ key, line: keyTok.line, column: keyTok.column });
                this.skipLayerValue();
            }
            if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
        }
        this.eat(TokenType.CloseParen);

        if (unknown.length > 0) tag.unknownKeys = unknown;
        return tag;
    }

    /**
     * 解析 @layer 中的有符号数值。
     *
     * 必须有符号：`shuttle_strategy=-1`（auto，**文档化的默认值**）与
     * `shuttle_lambda=-1`（须报 E-SHU003 而非崩溃）都依赖它。
     * 词法层把负号独立为 `Minus` token，故此处显式消费。
     *
     * 注：语义层负责拒绝无语义的负数（如 `time`/`thread`/`cost`，报 E-TOP009）——
     * 把「语法上可写」与「语义上合法」分开，才能给出带错误码的诊断
     * 而不是一个无码的 Parser Error 异常。
     */
    private parseLayerSignedNumber(): number {
        let sign = 1;
        if ((this.currentToken.type as TokenType) === TokenType.Minus) {
            this.eat(TokenType.Minus);
            sign = -1;
        }
        return sign * Number(this.eat(TokenType.Number).value);
    }

    /**
     * 解析 @layer 标量键的取值：数值、或 `true` / `false`。
     *
     * `true`/`false` 仅对 `dual` 有语义（对偶标记）。此前 `dual=true` 会抛
     * **无错误码的 Parser Error** 崩溃 —— 与 E-TOP008 修掉的静默丢键同类：
     * 用户拿不到可定位的诊断。此处把布尔字面量转入数值域（true=1/false=0）。
     */
    private parseLayerScalarValue(): number {
        const t = this.currentToken.type as TokenType;
        if (t === TokenType.Identifier || t === TokenType.Keyword) {
            const word = this.currentToken.value;
            if (word === 'true') { this.advance(); return 1; }
            if (word === 'false') { this.advance(); return 0; }
        }
        return this.parseLayerSignedNumber();
    }

    /**
     * 解析 @layer 中的数值列表 `(1, 0, 2)`，用于 `coord`。
     * 元素可带符号（负号在词法层是独立 token）。
     */
    private parseLayerNumberList(): number[] {
        this.eat(TokenType.OpenParen);
        const out = this.parseLayerNumberListBody();
        this.eat(TokenType.CloseParen);
        return out;
    }

    /** 已消费开括号后的数值序列体，停在 `)` 之前 */
    private parseLayerNumberListBody(): number[] {
        const out: number[] = [];
        while ((this.currentToken.type as TokenType) !== TokenType.CloseParen && (this.currentToken.type as TokenType) !== TokenType.EOF) {
            out.push(this.parseLayerSignedNumber());
            if ((this.currentToken.type as TokenType) === TokenType.Comma) this.eat(TokenType.Comma);
        }
        return out;
    }

    /**
     * 解析坐标**组**列表，用于 `shuttle`：
     *   `shuttle=(1,0)`            → [[1,0]]          （一个目标 coord）
     *   `shuttle=((1,0),(0,1))`    → [[1,0],[0,1]]    （两个目标 coord）
     *
     * 判据是外层括号内**首个元素是否为 `(`**：
     *   - 是 → 元素各为一个独立坐标群；
     *   - 否 → 整个外层列表就是**一个** coord。
     *
     * 这两者的区别不能靠展平来「简化」：把 `(1,0)` 当成两个群会得到
     * 目标 `[1]` 与 `[0]`（静默连错线，且只在目标块恰好存在时才不报错），
     * 而把 `((1,0),(0,1))` 展平成 `(1,0,0,1)` 会得到一个 4 维 coord。
     * 两种都是不报错、只连错线的偏差。
     */
    private parseLayerCoordGroups(): number[][] {
        if ((this.currentToken.type as TokenType) !== TokenType.OpenParen) {
            return [[this.parseLayerSignedNumber()]];
        }
        // 当前 token 为 '('，peek() 即括号内首个元素
        const nested = (this.peek().type as TokenType) === TokenType.OpenParen;
        this.eat(TokenType.OpenParen);
        const groups: number[][] = [];
        if (nested) {
            while ((this.currentToken.type as TokenType) !== TokenType.CloseParen && (this.currentToken.type as TokenType) !== TokenType.EOF) {
                groups.push(this.parseLayerNumberList());
                if ((this.currentToken.type as TokenType) === TokenType.Comma) this.eat(TokenType.Comma);
            }
        } else {
            groups.push(this.parseLayerNumberListBody());
        }
        this.eat(TokenType.CloseParen);
        return groups;
    }

    /**
     * 跳过 @layer 中未知键的一个值，保持解析进度。
     * 支持标量、字符串、`(...)` 列表三种形态。
     */
    private skipLayerValue(): void {
        let depth = 0;
        do {
            const t = this.currentToken.type as TokenType;
            if (t === TokenType.OpenParen || t === TokenType.OpenBracket) {
                depth++;
                this.eat(t);
            } else if (t === TokenType.CloseParen || t === TokenType.CloseBracket) {
                depth--;
                this.eat(t);
            } else if (t === TokenType.EOF) {
                return;
            } else {
                this.advance();
            }
        } while (depth > 0);
    }

    private parseTopLevelItem(): Statement | Item {
        // 函数属性（@[...]）与运行层标签（@layer(...)）作用于其后的顶层函数声明。
        // 二者可任意顺序混排：@[section("...")] @layer(...) fn 或 @layer(...) @[naked] fn。
        if (this.currentToken.type === TokenType.At) {
            let layer: LayerTag | undefined;
            const attributes: FunctionAttribute[] = [];
            while (this.currentToken.type === TokenType.At) {
                const next = this.peek();
                if (next.type === TokenType.Identifier && next.value === 'layer') {
                    layer = this.parseLayerTag();
                } else {
                    attributes.push(...this.parseAttributes());
                }
            }
            // @[packed] form / @[packed] pub form / @[packed] export form
            if (this.isKeyword('form')) {
                const formDecl = this.parseForm(false, false);
                formDecl.packed = attributes.some(a => a.name === 'packed');
                return formDecl;
            }
            if (this.isKeyword('pub') && this.peek().type === TokenType.Keyword && this.peek().value === 'form') {
                this.eatKeyword('pub');
                const formDecl = this.parseForm(true, false);
                formDecl.packed = attributes.some(a => a.name === 'packed');
                return formDecl;
            }
            if (this.isKeyword('export') && this.peek().type === TokenType.Keyword && this.peek().value === 'form') {
                this.eatKeyword('export');
                const formDecl = this.parseForm(false, true);
                formDecl.packed = attributes.some(a => a.name === 'packed');
                return formDecl;
            }

            const fn = this.parseDeclarationOrFunction(false, false) as FunctionDeclaration;
            fn.layer = layer;
            // 分离量子门属性与物理特性（与系统属性 @[section]/@[inline] 等正交）
            const { quantum, system: sysAfterQuantum } = extractQuantumAttrs(attributes);
            const { physical, system } = extractPhysicalAttrs(sysAfterQuantum);
            fn.attributes = system;
            fn.quantum = quantum;
            fn.physical = physical;
            return fn;
        }
        if (this.isKeyword('mod')) return this.parseModule();
        if (this.isKeyword('use')) return this.parseUse();
        if (this.isKeyword('form')) return this.parseForm(false, false);
        if (this.isKeyword('flavor')) return this.parseFlavor(false);
        if (this.isKeyword('impl')) return this.parseImpl();
        if (this.isKeyword('trait')) return this.parseTrait(false, false);
        if (this.isKeyword('template')) return this.parseTemplate();
        if (this.isKeyword('import')) return this.parseImport();
        if (this.isKeyword('extern')) return this.parseExtern();
        if (this.isKeyword('requires')) return this.parseRequires();
        if (this.isKeyword('export')) {
            this.eatKeyword('export');
            if (this.isKeyword('form')) return this.parseForm(false, true);
            if (this.isKeyword('trait')) return this.parseTrait(false, true);
            return this.parseDeclarationOrFunction(false, true);
        }
        if (this.isKeyword('pub')) {
            this.eatKeyword('pub');
            if (this.isKeyword('form')) return this.parseForm(true, false);
            if (this.isKeyword('trait')) return this.parseTrait(true, false);
            if (this.isKeyword('mod')) return this.parseModule();
            return this.parseDeclarationOrFunction(true, false);
        }
        // 顶层「显式类型」/ fixed 开头的声明：函数 或 全局变量 / 常量。
        // 类型 + 名字 + ( → 函数；类型 + 名字 + = → 全局变量（GlobalVarDecl）。
        // 注：let / auto 是类型推断，用于脚本模式的局部变量，不在此列。
        if (this.currentToken.type === TokenType.Keyword &&
            ALLOWED_TYPES.includes(this.currentToken.value) &&
            this.currentToken.value !== 'let' && this.currentToken.value !== 'auto') {
            const decl = this.parseDeclarationOrFunction(false, false);
            if (decl.type === 'VariableDeclaration') {
                const v = decl as VariableDeclaration;
                return {
                    type: 'GlobalVarDecl',
                    name: v.identifier,
                    varType: v.varType,
                    isFixed: v.isFixed,
                    value: v.value,
                    isPub: false,
                    line: v.line,
                    column: v.column,
                    length: v.length
                } as GlobalVarDecl;
            }
            // 一行多定义（`int32 a = 1, b = 2;`）：作为顶层 VariableDeclarationList（全局变量列表）返回
            return decl;
        }
        return this.parseStatement();
    }

    private parseImport(): ImportDecl {
        const importToken = this.eatKeyword('import');
        const alias = this.eatIdentifierOrKeyword().value;
        this.eatKeyword('from');
        const pathToken = this.eat(TokenType.String);
        this.consumeOptionalSemicolon();
        return {
            type: 'ImportDecl',
            alias: alias,
            path: pathToken.value,
            line: importToken.line,
            column: importToken.column,
            length: pathToken.value.length + 2
        } as ImportDecl;
    }

    // extern <ret> <name>(<params>); —— 外部 C 符号声明（FFI）
    private parseExtern(): ExternDecl {
        const externToken = this.eatKeyword('extern');
        const returnType = this.parseTypeRef();
        const name = this.eatIdentifierOrKeyword().value;
        const { params } = this.parseFunctionParams();
        this.consumeOptionalSemicolon();
        return {
            type: 'ExternDecl',
            returnType: returnType,
            name: name,
            params: params,
            line: externToken.line,
            column: externToken.column,
            length: this.currentToken.column - externToken.column
        } as ExternDecl;
    }

    private parseRequires(): RequiresDecl {
        const requiresToken = this.eatKeyword('requires');
        const segs = [this.eatIdentifierOrKeyword().value];
        while (this.currentToken.type === TokenType.Dot) {
            this.eat(TokenType.Dot);
            segs.push(this.eatIdentifierOrKeyword().value);
        }
        this.consumeOptionalSemicolon();
        const permission = segs.join('.');
        return {
            type: 'RequiresDecl',
            permission: permission,
            line: requiresToken.line,
            column: requiresToken.column,
            length: permission.length
        } as RequiresDecl;
    }

    private parseStatement(): Statement {
        const tokenType = this.currentToken.type;
        const tokenValue = this.currentToken.value;

        if (tokenType === TokenType.Keyword && ALLOWED_TYPES.includes(tokenValue)) {
            return this.parseDeclarationOrFunction(false);
        }

        if (tokenType === TokenType.Identifier && this.peek().type === TokenType.Identifier) {
            return this.parseDeclarationOrFunction(false);
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'while') {
            return this.parseWhileStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'for') {
            return this.parseForStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'if') {
            return this.parseIfStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'unsafe') {
            return this.parseUnsafeBlock();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'route') {
            return this.parseRouteStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'spin') {
            return this.parseSpinStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'spawn') {
            return this.parseSpawnStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'entangle') {
            return this.parseEntangleStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'asm') {
            return this.parseAsmStatement();
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'break') {
            const breakToken = this.eat(TokenType.Keyword);
            this.consumeOptionalSemicolon();
            return {
                type: 'BreakStatement',
                line: breakToken.line,
                column: breakToken.column,
                length: breakToken.length
            } as BreakStatement;
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'continue') {
            const contToken = this.eat(TokenType.Keyword);
            this.consumeOptionalSemicolon();
            return {
                type: 'ContinueStatement',
                line: contToken.line,
                column: contToken.column,
                length: contToken.length
            } as ContinueStatement;
        }

        if (tokenType === TokenType.Keyword && tokenValue === 'return') {
            return this.parseReturnStatement();
        }

        // `*p = v`：指针解引用赋值
        if (tokenType === TokenType.Star) {
            const expr = this.parseExpression();
            if (this.currentToken.type === TokenType.Equals) {
                this.eat(TokenType.Equals);
                const value = this.parseExpression();
                this.consumeOptionalSemicolon();
                return {
                    type: 'AssignmentStatement',
                    name: '',
                    target: expr,
                    value: value,
                    line: expr.line,
                    column: expr.column,
                    length: value.column + value.length - expr.column
                } as AssignmentStatement;
            }
            this.consumeOptionalSemicolon();
            return {
                type: 'ExpressionStatement',
                expression: expr,
                line: expr.line,
                column: expr.column,
                length: expr.length
            } as ExpressionStatement;
        }

        const isBuiltinCall = tokenType === TokenType.Keyword && BUILTIN_FUNCTIONS.includes(tokenValue);

        if (tokenType === TokenType.Identifier || isBuiltinCall) {
            const expr = this.parseExpression();
            if (this.currentToken.type === TokenType.Equals) {
                this.eat(TokenType.Equals);
                const value = this.parseExpression();
                this.consumeOptionalSemicolon();

                return {
                    type: 'AssignmentStatement',
                    name: (expr.type === 'MemberExpression' || expr.type === 'IndexExpression') ? '' : (expr as Identifier).name,
                    target: (expr.type === 'MemberExpression' || expr.type === 'IndexExpression') ? expr : undefined,
                    value: value,
                    line: expr.line,
                    column: expr.column,
                    length: value.column + value.length - expr.column
                } as AssignmentStatement;
            }
            this.consumeOptionalSemicolon();
            return {
                type: 'ExpressionStatement',
                expression: expr,
                line: expr.line,
                column: expr.column,
                length: expr.length
            } as ExpressionStatement;
        }

        throw new Error(`Parser Error: Unexpected token '${this.currentToken.value}' at line ${this.currentToken.line}, col ${this.currentToken.column}`);
    }

    private parseDeclarationOrFunction(isPub: boolean, isExport: boolean = false): Statement {
        const typeToken = this.eatIdentifierOrKeyword();
        let varType = typeToken.value;
        // fixed 修饰符：编译期常量（确定型范式）
        let isFixed = false;
        if (varType === 'fixed') {
            isFixed = true;
            varType = this.eatIdentifierOrKeyword().value;
        }
        if (varType === 'let' || varType === 'auto') varType = 'auto';

        // 泛型类型：cap<T>（能力）/ lattice<T, B>（晶格数组）/ arr<T, N>（定长数组）/ fn<ret(params)>（函数指针）
        if ((varType === 'cap' || varType === 'lattice') && this.currentToken.type === TokenType.LessThan) {
            varType = this.parseGenericTypeRef(varType);
        } else if (varType === 'arr' && this.currentToken.type === TokenType.LessThan) {
            varType = this.parseArrTypeRef();
        } else if (varType === 'fn' && this.currentToken.type === TokenType.LessThan) {
            varType = this.parseFnTypeRef();
        }

        const idToken = this.eatIdentifierOrKeyword();
        const identifier = idToken.value;

        if (this.currentToken.type === TokenType.OpenParen) {
            const { receiver, params } = this.parseFunctionParams();

            const requires: Expression[] = [];
            const ensures: Expression[] = [];
            while (this.isKeyword('requires') || this.isKeyword('ensures')) {
                const isEnsures = this.isKeyword('ensures');
                this.eat(TokenType.Keyword);
                const cond = this.parseExpression();
                this.consumeOptionalSemicolon();
                if (isEnsures) ensures.push(cond);
                else requires.push(cond);
            }

            this.eat(TokenType.OpenBrace);
            const body = this.parseBlock();

            return {
                type: 'FunctionDeclaration',
                returnType: varType,
                name: identifier,
                params: params,
                receiver: receiver,
                isPub: isPub,
                isExport: isExport,
                requires: requires,
                ensures: ensures,
                body: body,
                line: typeToken.line,
                column: typeToken.column,
                length: this.currentToken.column - typeToken.column
            } as FunctionDeclaration;
        }

        // 变量声明：支持一行多定义 `T a = e1, b = e2, c = e3;`
        const declarations: VariableDeclaration[] = [];
        let name = identifier;
        while (true) {
            this.eat(TokenType.Equals);
            const value = this.parseExpression();
            declarations.push({
                type: 'VariableDeclaration',
                varType: varType,
                identifier: name,
                value: value,
                isFixed: isFixed,
                line: typeToken.line,
                column: typeToken.column,
                length: value.column + value.length - typeToken.column
            } as VariableDeclaration);

            if (this.currentToken.type !== TokenType.Comma) break;
            this.eat(TokenType.Comma);
            name = this.eatIdentifierOrKeyword().value;
        }
        this.consumeOptionalSemicolon();

        if (declarations.length === 1) return declarations[0];

        const last = declarations[declarations.length - 1];
        return {
            type: 'VariableDeclarationList',
            varType: varType,
            isFixed: isFixed,
            declarations: declarations,
            line: typeToken.line,
            column: typeToken.column,
            length: last.value.column + last.value.length - typeToken.column
        } as VariableDeclarationList;
    }

    private parseReturnStatement(): ReturnStatement {
        const retToken = this.eat(TokenType.Keyword);
        // void 空返回：return;
        if (this.currentToken.type === TokenType.Semicolon) {
            this.eat(TokenType.Semicolon);
            return {
                type: 'ReturnStatement',
                argument: {
                    type: 'NumberLiteral',
                    value: 0,
                    line: retToken.line,
                    column: retToken.column,
                    length: 0
                } as any,
                isVoid: true,
                line: retToken.line,
                column: retToken.column,
                length: 7
            } as ReturnStatement;
        }
        const value = this.parseExpression();
        this.consumeOptionalSemicolon();

        return {
            type: 'ReturnStatement',
            argument: value,
            line: retToken.line,
            column: retToken.column,
            length: value.column + value.length - retToken.column
        } as ReturnStatement;
    }

    private parseWhileStatement(): WhileStatement {
        const whileToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenParen);
        const condition = this.parseExpression();
        this.eat(TokenType.CloseParen);

        const invariant: Expression[] = [];
        while (this.isKeyword('invariant')) {
            this.eat(TokenType.Keyword);
            invariant.push(this.parseExpression());
            this.consumeOptionalSemicolon();
        }

        this.eat(TokenType.OpenBrace);

        const body = this.parseBlock();
        const closeBrace = this.currentToken;

        let elseBody: Statement[] | undefined;
        if (this.isKeyword('else')) {
            this.eatKeyword('else');
            this.eat(TokenType.OpenBrace);
            elseBody = this.parseBlock();
        }

        return {
            type: 'WhileStatement',
            condition: condition,
            body: body,
            elseBody: elseBody,
            invariant: invariant,
            line: whileToken.line,
            column: whileToken.column,
            length: closeBrace.column - whileToken.column
        } as WhileStatement;
    }

    /**
     * if / else if / else
     *
     * `else if` 解析为「else 分支只含一个 IfStatement」，而非单独的
     * ElseIf 节点。这样 IR / MIR 的 lowering 只需处理一种分支形态，
     * 不必为链式 else-if 写特判。
     */
    private parseIfStatement(): IfStatement {
        const ifToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenParen);
        const condition = this.parseExpression();
        this.eat(TokenType.CloseParen);

        this.eat(TokenType.OpenBrace);
        const consequent = this.parseBlock();

        let alternate: Statement[] | null = null;
        if (this.isKeyword('else')) {
            this.eatKeyword('else');
            if (this.isKeyword('if')) {
                // else if —— 递归解析，包成单元素数组
                alternate = [this.parseIfStatement()];
            } else {
                this.eat(TokenType.OpenBrace);
                alternate = this.parseBlock();
            }
        }

        return {
            type: 'IfStatement',
            condition: condition,
            consequent: consequent,
            alternate: alternate,
            line: ifToken.line,
            column: ifToken.column,
            length: ifToken.length
        } as IfStatement;
    }

    /**
     * unsafe { ... } —— 系统级危险操作块（裸指针解引用 / MMIO / 内联汇编）。
     * 是"写内核"的显式危险边界；能力校验在语义层完成。
     */
    private parseUnsafeBlock(): UnsafeBlock {
        const unsafeToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenBrace);
        const body = this.parseBlock();
        return {
            type: 'UnsafeBlock',
            body: body,
            line: unsafeToken.line,
            column: unsafeToken.column,
            length: unsafeToken.length
        } as UnsafeBlock;
    }

    /**
     * route (discriminant) { path V: {...} fallback: {...} }
     * 路由分支：把判别值路由到若干路径之一。
     */
    private parseRouteStatement(): RouteStatement {
        const routeToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenParen);
        const discriminant = this.parseExpression();
        this.eat(TokenType.CloseParen);
        this.eat(TokenType.OpenBrace);

        const cases: RouteCase[] = [];
        let fallback: Statement[] | null = null;

        while (this.currentToken.type !== TokenType.CloseBrace) {
            if (this.isKeyword('path')) {
                this.eatKeyword('path');
                const value = this.parseExpression();
                this.eat(TokenType.Colon);
                this.eat(TokenType.OpenBrace);
                const body = this.parseBlock();
                cases.push({ value, body });
            } else if (this.isKeyword('fallback')) {
                this.eatKeyword('fallback');
                this.eat(TokenType.Colon);
                this.eat(TokenType.OpenBrace);
                fallback = this.parseBlock();
            } else {
                throw new Error(`Parser Error: expected 'path' or 'fallback' in route statement (line ${routeToken.line})`);
            }
        }
        this.eat(TokenType.CloseBrace);

        return {
            type: 'RouteStatement',
            discriminant,
            cases,
            fallback,
            line: routeToken.line,
            column: routeToken.column,
            length: routeToken.length
        } as RouteStatement;
    }

    /**
     * spin { body } while (cond);
     * 自旋循环：先执行循环体，再判断条件，至少执行一次。
     */
    private parseSpinStatement(): SpinStatement {
        const spinToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenBrace);
        const body = this.parseBlock();
        this.eatKeyword('while');
        this.eat(TokenType.OpenParen);
        const condition = this.parseExpression();
        this.eat(TokenType.CloseParen);
        this.consumeOptionalSemicolon();

        return {
            type: 'SpinStatement',
            body,
            condition,
            line: spinToken.line,
            column: spinToken.column,
            length: spinToken.length
        } as SpinStatement;
    }

    /**
     * spawn { ... } —— 派生并发线程（Q-Digest）。
     * 块体闭包继承外层作用域，是竞争检测的"线程"单元。
     */
    private parseSpawnStatement(): SpawnStatement {
        const spawnToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenBrace);
        const body = this.parseBlock();
        return {
            type: 'SpawnStatement',
            body,
            line: spawnToken.line,
            column: spawnToken.column,
            length: spawnToken.length
        } as SpawnStatement;
    }

    /**
     * asm { "..." "..." } —— 裸汇编块（多指令 / 多寄存器序列）。
     * 块内为一个或多个字符串字面量，按行拼接为单一汇编模板；
     * 不做 ${} 占位符替换（区别于 native()），供 @[naked] 裸函数与中断存根使用。
     */
    private parseAsmStatement(): AsmStatement {
        const asmToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenBrace);
        const lines: string[] = [];
        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            if (this.currentToken.type === TokenType.String) {
                lines.push(this.eat(TokenType.String).value);
            } else if (this.currentToken.type === TokenType.Semicolon) {
                this.eat(TokenType.Semicolon);
            } else {
                throw new Error(`Parser Error: expected string literal in asm block (line ${this.currentToken.line})`);
            }
        }
        this.eat(TokenType.CloseBrace);
        return {
            type: 'AsmStatement',
            template: lines.join('\n'),
            line: asmToken.line,
            column: asmToken.column,
            length: asmToken.length
        } as AsmStatement;
    }

    /**
     * entangle(q1, q2) —— 量子纠缠声明（Q-Digest）。
     * 纠缠具有传递性，竞争检测据此构建纠缠闭包。
     */
    private parseEntangleStatement(): EntangleStatement {
        const entangleToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenParen);
        const left = this.parseExpression();
        this.eat(TokenType.Comma);
        const right = this.parseExpression();
        this.eat(TokenType.CloseParen);
        this.consumeOptionalSemicolon();
        return {
            type: 'EntangleStatement',
            left,
            right,
            line: entangleToken.line,
            column: entangleToken.column,
            length: entangleToken.length
        } as EntangleStatement;
    }

    private parseForStatement(): ForStatement {
        const forToken = this.eat(TokenType.Keyword);
        this.eat(TokenType.OpenParen);

        let init: Statement | null = null;
        if (this.currentToken.type !== TokenType.Semicolon) {
            init = this.parseForClauseStatement();
        }
        this.eat(TokenType.Semicolon);

        let condition: Expression | null = null;
        if (this.currentToken.type !== TokenType.Semicolon) {
            condition = this.parseExpression();
        }
        this.eat(TokenType.Semicolon);

        let update: Statement | null = null;
        if (this.currentToken.type !== TokenType.CloseParen) {
            update = this.parseForClauseStatement();
        }
        this.eat(TokenType.CloseParen);

        const invariant: Expression[] = [];
        while (this.isKeyword('invariant')) {
            this.eat(TokenType.Keyword);
            invariant.push(this.parseExpression());
            this.consumeOptionalSemicolon();
        }

        this.eat(TokenType.OpenBrace);
        const body = this.parseBlock();

        return {
            type: 'ForStatement',
            init: init,
            condition: condition,
            update: update,
            body: body,
            invariant: invariant,
            line: forToken.line,
            column: forToken.column,
            length: this.currentToken.column - forToken.column
        } as ForStatement;
    }

    private parseForClauseStatement(): Statement {
        const tokenType = this.currentToken.type;
        const tokenValue = this.currentToken.value;
        if (tokenType === TokenType.Keyword && ALLOWED_TYPES.includes(tokenValue)) {
            const typeToken = this.eatIdentifierOrKeyword();
            const varType = (typeToken.value === 'let' || typeToken.value === 'auto') ? 'auto' : typeToken.value;
            const idToken = this.eatIdentifierOrKeyword();
            this.eat(TokenType.Equals);
            const value = this.parseExpression();
            return {
                type: 'VariableDeclaration',
                varType: varType,
                identifier: idToken.value,
                value: value,
                line: typeToken.line,
                column: typeToken.column,
                length: value.column + value.length - typeToken.column
            } as VariableDeclaration;
        }
        const expr = this.parseExpression();
        this.eat(TokenType.Equals);
        const value = this.parseExpression();
        return {
            type: 'AssignmentStatement',
            name: expr.type === 'MemberExpression' ? '' : (expr as Identifier).name,
            target: expr.type === 'MemberExpression' ? expr : undefined,
            value: value,
            line: expr.line,
            column: expr.column,
            length: value.column + value.length - expr.column
        } as AssignmentStatement;
    }

    private parseBlock(): Statement[] {
        const body: Statement[] = [];
        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            body.push(this.parseStatement());
        }
        this.eat(TokenType.CloseBrace);
        return body;
    }

    private parseExpression(): Expression {
        return this.parseLogical();
    }

    private parseLogical(): Expression {
        let left = this.parseComparison();

        while (
            this.currentToken.type === TokenType.AndAnd ||
            this.currentToken.type === TokenType.OrOr
        ) {
            const opToken = this.advance();
            const right = this.parseComparison();
            left = {
                type: 'LogicalExpression',
                operator: opToken.value,
                left: left,
                right: right,
                line: left.line,
                column: left.column,
                length: right.column + right.length - left.column
            } as LogicalExpression;
        }
        return left;
    }

    // 比较层：< <= > >= == !=（优先级低于算术，高于逻辑）
    private parseComparison(): Expression {
        let left = this.parseAdditive();

        while (
            this.currentToken.type === TokenType.LessThan ||
            this.currentToken.type === TokenType.LessEqual ||
            this.currentToken.type === TokenType.GreaterThan ||
            this.currentToken.type === TokenType.GreaterEqual ||
            this.currentToken.type === TokenType.EqualsEquals ||
            this.currentToken.type === TokenType.NotEqual
        ) {
            const opToken = this.advance();
            const right = this.parseAdditive();
            left = {
                type: 'BinaryExpression',
                operator: opToken.value,
                left: left,
                right: right,
                line: left.line,
                column: left.column,
                length: right.column + right.length - left.column
            } as BinaryExpression;
        }
        return left;
    }

    // 加减层：+ -（优先级低于乘除，高于比较）
    private parseAdditive(): Expression {
        let left = this.parseMultiplicative();

        while (
            this.currentToken.type === TokenType.Plus ||
            this.currentToken.type === TokenType.Minus
        ) {
            const opToken = this.advance();
            const right = this.parseMultiplicative();
            left = {
                type: 'BinaryExpression',
                operator: opToken.value,
                left: left,
                right: right,
                line: left.line,
                column: left.column,
                length: right.column + right.length - left.column
            } as BinaryExpression;
        }
        return left;
    }

    private parseMultiplicative(): Expression {
        let left = this.parseUnary();

        while (
            this.currentToken.type === TokenType.Star ||
            this.currentToken.type === TokenType.Slash ||
            this.currentToken.type === TokenType.Percent ||
            this.currentToken.type === TokenType.Ampersand ||
            this.currentToken.type === TokenType.Pipe ||
            this.currentToken.type === TokenType.Caret ||
            this.currentToken.type === TokenType.ShiftLeft ||
            this.currentToken.type === TokenType.ShiftRight
        ) {
            const opToken = this.advance();
            const right = this.parseUnary();
            left = {
                type: 'BinaryExpression',
                operator: opToken.value,
                left: left,
                right: right,
                line: left.line,
                column: left.column,
                length: right.column + right.length - left.column
            } as BinaryExpression;
        }
        return left;
    }

    private parseUnary(): Expression {
        if (this.currentToken.type === TokenType.Bang ||
            this.currentToken.type === TokenType.Minus ||
            this.currentToken.type === TokenType.Tilde ||
            this.currentToken.type === TokenType.Star ||
            this.currentToken.type === TokenType.Ampersand) {
            const opToken = this.advance();
            // `&` 后若紧跟内置函数 Keyword（如 rvdl_sys_log / rvdl_gc_alloc），应视为
            // 标识符取地址，而非内置函数调用（否则 parseArgs 期望 '(' 报错）。
            let argument: Expression;
            if (opToken.type === TokenType.Ampersand &&
                (this.currentToken.type as TokenType) === TokenType.Keyword &&
                BUILTIN_FUNCTIONS.includes(this.currentToken.value)) {
                const kw = this.eat(TokenType.Keyword);
                argument = {
                    type: 'Identifier',
                    name: kw.value,
                    line: kw.line,
                    column: kw.column,
                    length: kw.length
                } as any;
            } else {
                argument = this.parseUnary();
            }
            // `*` 是前缀解引用
            if (opToken.type === TokenType.Star) {
                return {
                    type: 'Dereference',
                    target: argument,
                    line: opToken.line,
                    column: opToken.column,
                    length: argument.column + argument.length - opToken.column
                } as Dereference;
            }
            // `&` 是取地址（函数指针）
            if (opToken.type === TokenType.Ampersand) {
                return {
                    type: 'AddressOf',
                    target: argument,
                    line: opToken.line,
                    column: opToken.column,
                    length: argument.column + argument.length - opToken.column
                } as AddressOf;
            }
            return {
                type: 'UnaryExpression',
                operator: opToken.value,
                argument: argument,
                line: opToken.line,
                column: opToken.column,
                length: argument.column + argument.length - opToken.column
            } as UnaryExpression;
        }
        return this.parsePrimary();
    }

    private parsePrimary(): Expression {
        const token = this.currentToken;
        let left: Expression;

        if (token.type === TokenType.OpenParen) {
            this.eat(TokenType.OpenParen);
            const inner = this.parseExpression();
            this.eat(TokenType.CloseParen);
            return inner;
        }

        if (token.type === TokenType.Number) {
            const numToken = this.eat(TokenType.Number);
            left = {
                type: 'NumberLiteral',
                value: Number(numToken.value),
                isFloat: (!/^0[xX]/.test(numToken.value)) && (numToken.value.includes('.') || /[eE]/.test(numToken.value)),
                raw: numToken.value,
                line: numToken.line,
                column: numToken.column,
                length: numToken.length
            } as NumberLiteral;
        }
        else if (token.type === TokenType.String) {
            const strToken = this.eat(TokenType.String);
            left = {
                type: 'StringLiteral',
                value: strToken.value,
                line: strToken.line,
                column: strToken.column,
                length: strToken.length
            } as StringLiteral;
        }
        else if (token.type === TokenType.Keyword && token.value === 'null') {
            const nullToken = this.eat(TokenType.Keyword);
            left = {
                type: 'NullLiteral',
                line: nullToken.line,
                column: nullToken.column,
                length: nullToken.length
            } as NullLiteral;
        }
        else if (token.type === TokenType.Keyword && token.value === 'native') {
            this.eat(TokenType.Keyword);
            this.eat(TokenType.OpenParen);
            const tplToken = this.eat(TokenType.String);
            // 可选操作数：native("模板", op1, op2, ...)
            const operands: Expression[] = [];
            while (this.currentToken.type === TokenType.Comma) {
                this.eat(TokenType.Comma);
                operands.push(this.parseExpression());
            }
            this.eat(TokenType.CloseParen);
            left = {
                type: 'NativeExpression',
                template: tplToken.value,
                operands,
                line: token.line,
                column: token.column,
                length: tplToken.value.length + 6
            } as NativeExpression;
        }
        else if (token.type === TokenType.Keyword && token.value === 'fuse') {
            this.eat(TokenType.Keyword);
            this.eat(TokenType.OpenParen);
            const discriminant = this.parseExpression();
            this.eat(TokenType.CloseParen);
            this.eat(TokenType.OpenBrace);
            const arms: FuseArm[] = [];
            while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
                let pattern: Expression | null;
                if (this.currentToken.type === TokenType.Identifier && this.currentToken.value === '_') {
                    this.eat(TokenType.Identifier);
                    pattern = null;
                } else {
                    pattern = this.parseExpression();
                }
                this.eat(TokenType.Colon);
                const value = this.parseExpression();
                arms.push({ pattern, value });
                if (this.currentToken.type === TokenType.Comma) {
                    this.eat(TokenType.Comma);
                }
            }
            this.eat(TokenType.CloseBrace);

            left = {
                type: 'FuseExpression',
                discriminant,
                arms,
                line: token.line,
                column: token.column,
                length: this.currentToken.column - token.column
            } as FuseExpression;
        }
        else if (token.type === TokenType.Keyword && token.value === 'new') {
            const newToken = this.eat(TokenType.Keyword);
            let className = this.eatIdentifierOrKeyword().value;

            while (this.currentToken.type === TokenType.ColonColon) {
                this.eat(TokenType.ColonColon);
                className += '::' + this.eatIdentifierOrKeyword().value;
            }
            if (this.currentToken.type === TokenType.LessThan) {
                className += this.parseGenericTypeSuffix();
            }

            const args = this.parseArgs();
            const closeParen = this.currentToken;
            left = {
                type: 'NewExpression',
                className,
                arguments: args,
                line: newToken.line,
                column: newToken.column,
                length: closeParen.column - newToken.column
            } as NewExpression;
        }
        else if (token.type === TokenType.Keyword && token.value === 'make') {
            const mkToken = this.eat(TokenType.Keyword);
            let className = this.eatIdentifierOrKeyword().value;

            while (this.currentToken.type === TokenType.ColonColon) {
                this.eat(TokenType.ColonColon);
                className += '::' + this.eatIdentifierOrKeyword().value;
            }
            if (this.currentToken.type === TokenType.LessThan) {
                className += this.parseGenericTypeSuffix();
            }

            const args = this.parseArgs();
            const closeParen = this.currentToken;
            left = {
                type: 'NewExpression',
                className,
                arguments: args,
                heapAlloc: true,
                line: mkToken.line,
                column: mkToken.column,
                length: closeParen.column - mkToken.column
            } as NewExpression;
        }
        else if (token.type === TokenType.Keyword && token.value === 'fn') {
            const fnToken = this.eat(TokenType.Keyword);
            this.eat(TokenType.OpenParen);
            const params: Param[] = [];
            while (this.currentToken.type !== TokenType.CloseParen) {
                const type = this.parseTypeRef();
                const name = this.eatIdentifierOrKeyword().value;
                params.push({ name, type });
                if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
            }
            this.eat(TokenType.CloseParen);

            let returnType: string | null = null;
            if ((this.currentToken.type as TokenType) === TokenType.Arrow) {
                this.eat(TokenType.Arrow);
                returnType = this.parseTypeRef();
            }

            this.eat(TokenType.OpenBrace);
            const body = this.parseBlock();

            left = {
                type: 'FunctionExpression',
                params: params,
                returnType: returnType,
                body: body,
                line: fnToken.line,
                column: fnToken.column,
                length: this.currentToken.column - fnToken.column
            } as FunctionExpression;
        }
        else if (token.type === TokenType.Keyword && BUILTIN_FUNCTIONS.includes(token.value)) {
            const funcToken = this.eat(TokenType.Keyword);
            const funcName = funcToken.value;
            const args = this.parseArgs();
            const closeParen = this.currentToken;
            left = {
                type: 'FunctionCall',
                name: funcName,
                arguments: args,
                line: funcToken.line,
                column: funcToken.column,
                length: closeParen.column - funcToken.column
            } as FunctionCall;
        }
        else if (token.type === TokenType.Keyword && token.value === 'self') {
            const idToken = this.eat(TokenType.Keyword);
            left = {
                type: 'Identifier',
                name: 'self',
                line: idToken.line,
                column: idToken.column,
                length: idToken.length
            } as Identifier;
        }
        else if (token.type === TokenType.Keyword && token.value === 'result') {
            const resultToken = this.eat(TokenType.Keyword);
            left = {
                type: 'ResultExpr',
                line: resultToken.line,
                column: resultToken.column,
                length: resultToken.length
            } as ResultExpr;
        }
        else if (token.type === TokenType.OpenBracket) {
            // 数组字面量：[e0, e1, ...]（静态查找表 / 定长数组初始化）
            const openToken = this.eat(TokenType.OpenBracket);
            const elements: Expression[] = [];
            while (this.currentToken.type !== TokenType.CloseBracket && this.currentToken.type !== TokenType.EOF) {
                elements.push(this.parseExpression());
                if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
            }
            this.eat(TokenType.CloseBracket);
            left = {
                type: 'ArrayLiteral',
                elements,
                line: openToken.line,
                column: openToken.column,
                length: this.currentToken.column - openToken.column
            } as ArrayLiteral;
        }
        else if (token.type === TokenType.Identifier) {
            const idToken = this.eat(TokenType.Identifier);

            if (this.currentToken.type === TokenType.ColonColon) {
                const segs = [idToken.value];
                while (this.currentToken.type === TokenType.ColonColon) {
                    this.eat(TokenType.ColonColon);
                    segs.push(this.eatIdentifierOrKeyword().value);
                }
                if (this.currentToken.type === TokenType.OpenParen) {
                    const args = this.parseArgs();
                    const closeParen = this.currentToken;
                    left = {
                        type: 'FunctionCall',
                        name: segs.join('::'),
                        arguments: args,
                        line: idToken.line,
                        column: idToken.column,
                        length: closeParen.column - idToken.column
                    } as FunctionCall;
                } else {
                    left = {
                        type: 'Identifier',
                        name: segs.join('::'),
                        line: idToken.line,
                        column: idToken.column,
                        length: idToken.length
                    } as Identifier;
                }
            }
            else if (this.currentToken.type === TokenType.OpenParen) {
                const args = this.parseArgs();
                const closeParen = this.currentToken;
                left = {
                    type: 'FunctionCall',
                    name: idToken.value,
                    arguments: args,
                    line: idToken.line,
                    column: idToken.column,
                    length: closeParen.column - idToken.column
                } as FunctionCall;
            }
            else {
                left = {
                    type: 'Identifier',
                    name: idToken.value,
                    line: idToken.line,
                    column: idToken.column,
                    length: idToken.length
                } as Identifier;
            }
        }
        else {
            throw new Error(`Parser Error: Unexpected token '${this.currentToken.value}' at line ${token.line}, col ${token.column}`);
        }

        while (this.currentToken.type === TokenType.Dot) {
            this.eat(TokenType.Dot);
            const propToken = this.currentToken;
            if ((this.currentToken.type as TokenType) === TokenType.Identifier || (this.currentToken.type as TokenType) === TokenType.Keyword) {
                this.advance();
            } else {
                throw new Error(`Parser Error: Expected property name after dot at line ${this.currentToken.line}, col ${this.currentToken.column}`);
            }

            let isMethodCall = false;
            const args: Expression[] = [];
            let endCol = propToken.column + propToken.length;

            if ((this.currentToken.type as TokenType) === TokenType.OpenParen) {
                isMethodCall = true;
                args.push(...this.parseArgs());
                endCol = this.currentToken.column;
            }

            left = {
                type: 'MemberExpression',
                object: left,
                property: propToken.value,
                isMethodCall,
                arguments: args,
                line: left.line,
                column: left.column,
                length: endCol - left.column
            } as MemberExpression;
        }

        // 晶格索引：board[x, y]（多维下标访问）
        while (this.currentToken.type === TokenType.OpenBracket) {
            const indices = this.parseIndexList();
            const col = this.currentToken.column;
            left = {
                type: 'IndexExpression',
                object: left,
                indices: indices,
                line: left.line,
                column: left.column,
                length: col - left.column
            } as IndexExpression;
        }
        return left;
    }

    // 解析 [i0, i1, ...] 下标表（含方括号，消费到 ']' 为止）
    private parseIndexList(): Expression[] {
        this.eat(TokenType.OpenBracket);
        const indices: Expression[] = [];
        while (this.currentToken.type !== TokenType.CloseBracket && this.currentToken.type !== TokenType.EOF) {
            indices.push(this.parseExpression());
            if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
        }
        this.eat(TokenType.CloseBracket);
        return indices;
    }

    private parseArgs(): Expression[] {
        this.eat(TokenType.OpenParen);
        const args: Expression[] = [];
        while (this.currentToken.type !== TokenType.CloseParen) {
            args.push(this.parseExpression());
            if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
        }
        this.eat(TokenType.CloseParen);
        return args;
    }

    private parseTypeRef(): string {
        let result = this.eatIdentifierOrKeyword().value;
        while (this.currentToken.type === TokenType.ColonColon) {
            this.eat(TokenType.ColonColon);
            result += '::' + this.eatIdentifierOrKeyword().value;
        }
        // 泛型类型：cap<T> / lattice<T, B> / arr<T, N> / fn<ret(params)>
        if ((result === 'cap' || result === 'lattice') && this.currentToken.type === TokenType.LessThan) {
            result = this.parseGenericTypeRef(result);
        } else if (result === 'arr' && this.currentToken.type === TokenType.LessThan) {
            result = this.parseArrTypeRef();
        } else if (result === 'fn' && this.currentToken.type === TokenType.LessThan) {
            result = this.parseFnTypeRef();
        }
        return result;
    }

    /** 解析 arr<T, N> 类型（前置条件：'arr' 已消费、当前为 '<'），返回完整 'arr<T, N>' 字符串 */
    private parseArrTypeRef(): string {
        this.eat(TokenType.LessThan);
        const elem = this.parseTypeRef();
        this.eat(TokenType.Comma);
        const len = this.eat(TokenType.Number).value;
        this.eat(TokenType.GreaterThan);
        return `arr<${elem}, ${len}>`;
    }

    /** 解析 fn<ret(params)> 类型（前置条件：'fn' 已消费、当前为 '<'），返回规范 '(params)->ret' 字符串 */
    private parseFnTypeRef(): string {
        this.eat(TokenType.LessThan);
        const ret = this.parseTypeRef();
        const params: string[] = [];
        if ((this.currentToken.type as TokenType) === TokenType.OpenParen) {
            this.eat(TokenType.OpenParen);
            while ((this.currentToken.type as TokenType) !== TokenType.CloseParen && (this.currentToken.type as TokenType) !== TokenType.EOF) {
                params.push(this.parseTypeRef());
                if ((this.currentToken.type as TokenType) === TokenType.Comma) this.eat(TokenType.Comma);
            }
            this.eat(TokenType.CloseParen);
        }
        this.eat(TokenType.GreaterThan);
        return `(${params.join(', ')})->${ret}`;
    }

    /**
     * 解析 `<T1, T2, ...>` 泛型参数表（含尖括号，支持逗号分隔多参数）。
     * 前置条件：当前 token 为 '<'；消费到 '>' 为止。
     */
    private parseGenericTypeSuffix(): string {
        this.eat(TokenType.LessThan);
        const parts: string[] = [];
        do {
            parts.push(this.parseTypeRef());
            if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
        } while (this.currentToken.type !== TokenType.GreaterThan && this.currentToken.type !== TokenType.EOF);
        this.eat(TokenType.GreaterThan);
        return '<' + parts.join(', ') + '>';
    }

    /** 解析 base<T...> 并返回完整类型字符串（如 cap<int32>、lattice<int32, open>）。 */
    private parseGenericTypeRef(base: string): string {
        return base + this.parseGenericTypeSuffix();
    }

    private parsePath(): string[] {
        const segs = [this.eatIdentifierOrKeyword().value];
        while (this.currentToken.type === TokenType.ColonColon) {
            this.eat(TokenType.ColonColon);
            segs.push(this.eatIdentifierOrKeyword().value);
        }
        return segs;
    }

    private parseFunctionParams(): { receiver: string | null, params: Param[] } {
        this.eat(TokenType.OpenParen);
        let receiver: string | null = null;
        const params: Param[] = [];

        if (this.currentToken.type === TokenType.Ampersand) {
            this.eat(TokenType.Ampersand);
            this.eatKeyword('self');
            receiver = '&self';
        } else if (this.isKeyword('self')) {
            this.eatKeyword('self');
            receiver = 'self';
        }

        if (receiver && this.currentToken.type === TokenType.Comma) {
            this.eat(TokenType.Comma);
        }

        while (this.currentToken.type !== TokenType.CloseParen) {
            const type = this.parseTypeRef();
            const name = this.eatIdentifierOrKeyword().value;
            params.push({ name, type });
            if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
        }

        this.eat(TokenType.CloseParen);
        return { receiver, params };
    }

    private parseModule(): ModuleDecl {
        const modToken = this.eatKeyword('mod');
        const name = this.eatIdentifierOrKeyword().value;
        this.eat(TokenType.OpenBrace);

        const body: Item[] = [];
        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            body.push(this.parseTopLevelItem() as Item);
        }
        const closeBrace = this.eat(TokenType.CloseBrace);

        return {
            type: 'ModuleDecl',
            name: name,
            body: body,
            line: modToken.line,
            column: modToken.column,
            length: closeBrace.column - modToken.column
        };
    }

    private parseUse(): UseDecl {
        const useToken = this.eatKeyword('use');
        const path = this.parsePath();
        this.consumeOptionalSemicolon();
        return {
            type: 'UseDecl',
            path: path,
            line: useToken.line,
            column: useToken.column,
            length: path.join('::').length
        };
    }

    /**
     * flavor Name { A, B, C }
     * flavor Name { A = 1, B = 5, C }   // C 自动 = 6
     * 味：具名常量，自动按声明顺序赋值 0,1,2,...，或显式赋值。
     */
    private parseFlavor(isPub: boolean): FlavorDecl {
        const flavorToken = this.eatKeyword('flavor');
        const name = this.eatIdentifierOrKeyword().value;
        this.eat(TokenType.OpenBrace);
        const members: FlavorMember[] = [];
        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            const memberName = this.eatIdentifierOrKeyword().value;
            let value: number | null = null;
            if (this.currentToken.type === TokenType.Equals) {
                this.eat(TokenType.Equals);
                value = Number(this.eat(TokenType.Number).value);
            }
            members.push({ name: memberName, value });
            if (this.currentToken.type === TokenType.Comma) {
                this.eat(TokenType.Comma);
            }
        }
        this.eat(TokenType.CloseBrace);

        return {
            type: 'FlavorDecl',
            name,
            members,
            isPub,
            line: flavorToken.line,
            column: flavorToken.column,
            length: this.currentToken.column - flavorToken.column
        };
    }

    private parseForm(isPub: boolean, isExport: boolean = false): FormDecl {
        const formToken = this.eatKeyword('form');
        const name = this.eatIdentifierOrKeyword().value;
        const inherits = this.parseInheritClause();
        const ranks = this.parseFormBody();

        return {
            type: 'FormDecl',
            name: name,
            isPub: isPub,
            isExport: isExport,
            inherits: inherits,
            ranks: ranks,
            line: formToken.line,
            column: formToken.column,
            length: this.currentToken.column - formToken.column
        };
    }

    private parseTrait(isPub: boolean, isExport: boolean = false): TraitDecl {
        const traitToken = this.eatKeyword('trait');
        const name = this.eatIdentifierOrKeyword().value;
        const inherits = this.parseInheritClause();
        const ranks = this.parseTraitBody();

        return {
            type: 'TraitDecl',
            name: name,
            isPub: isPub,
            inherits: inherits,
            ranks: ranks,
            line: traitToken.line,
            column: traitToken.column,
            length: this.currentToken.column - traitToken.column
        };
    }

    private parseImpl(): ImplDecl {
        const implToken = this.eatKeyword('impl');
        let traitName: string | null = null;
        let target: string;

        const first = this.parsePath();
        if (this.isKeyword('for')) {
            this.eatKeyword('for');
            traitName = first.join('::');
            target = this.parsePath().join('::');
        } else {
            target = first.join('::');
        }

        let rank: string | null = null;
        if (this.currentToken.type === TokenType.Dot) {
            this.eat(TokenType.Dot);
            rank = this.eatIdentifierOrKeyword().value;
        }

        this.eat(TokenType.OpenBrace);
        const methods: FnDecl[] = [];
        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            const mPub = this.isKeyword('pub');
            if (mPub) this.eatKeyword('pub');
            methods.push(this.parseMethod());
        }
        const closeBrace = this.eat(TokenType.CloseBrace);

        return {
            type: 'ImplDecl',
            target: target,
            traitName: traitName,
            rank: rank,
            methods: methods,
            line: implToken.line,
            column: implToken.column,
            length: closeBrace.column - implToken.column
        };
    }

    private parseTemplate(): TemplateDecl {
        const templateToken = this.eatKeyword('template');
        this.eat(TokenType.LessThan);
        const params: string[] = [this.eatIdentifierOrKeyword().value];
        while (this.currentToken.type === TokenType.Comma) {
            this.eat(TokenType.Comma);
            params.push(this.eatIdentifierOrKeyword().value);
        }
        this.eat(TokenType.GreaterThan);

        let inner: FormDecl | ImplDecl;
        if (this.isKeyword('form')) {
            inner = this.parseForm(false);
        } else if (this.isKeyword('impl')) {
            inner = this.parseImpl();
        } else {
            throw new Error(`Parser Error: template must be followed by 'form' or 'impl', found '${this.currentToken.value}' at line ${this.currentToken.line}, col ${this.currentToken.column}`);
        }

        return {
            type: 'TemplateDecl',
            params: params,
            inner: inner,
            line: templateToken.line,
            column: templateToken.column,
            length: inner.length
        };
    }

    private parseInheritClause(): InheritClause | null {
        if (this.currentToken.type !== TokenType.Colon) return null;
        this.eat(TokenType.Colon);
        const base = this.parsePath().join('::');
        let ranks: string[] = [];
        if ((this.currentToken.type as TokenType) === TokenType.Dot) {
            this.eat(TokenType.Dot);
            this.eat(TokenType.OpenBrace);
            ranks = this.parseRankNameList();
            this.eat(TokenType.CloseBrace);
        }
        return { base, ranks };
    }

    private parseRankNameList(): string[] {
        const ranks: string[] = [this.eatIdentifierOrKeyword().value];
        while (this.currentToken.type === TokenType.Comma) {
            this.eat(TokenType.Comma);
            ranks.push(this.eatIdentifierOrKeyword().value);
        }
        return ranks;
    }

    private parseFormBody(): RankBlock[] {
        this.eat(TokenType.OpenBrace);
        const ranks: RankBlock[] = [];
        const defaultRank: RankBlock = { name: 'classical', fields: [], methods: [] };

        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            if (this.isKeyword('rank')) {
                this.eatKeyword('rank');
                const rankName = this.eatIdentifierOrKeyword().value;
                ranks.push(this.parseRankBlockBody(rankName));
            } else {
                const isPub = this.isKeyword('pub');
                if (isPub) this.eatKeyword('pub');
                this.parseFieldOrMethod(defaultRank, isPub);
            }
        }
        this.eat(TokenType.CloseBrace);

        if (defaultRank.fields.length > 0 || defaultRank.methods.length > 0) {
            ranks.unshift(defaultRank);
        }
        return ranks;
    }

    private parseRankBlockBody(rankName: string): RankBlock {
        this.eat(TokenType.OpenBrace);
        const block: RankBlock = { name: rankName, fields: [], methods: [] };
        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            const isPub = this.isKeyword('pub');
            if (isPub) this.eatKeyword('pub');
            this.parseFieldOrMethod(block, isPub);
        }
        this.eat(TokenType.CloseBrace);
        return block;
    }

    private parseFieldOrMethod(rank: RankBlock, isPub: boolean): void {
        const typeToken = this.parseTypeRef();
        const name = this.eatIdentifierOrKeyword().value;

        if (this.currentToken.type === TokenType.OpenParen) {
            const { receiver, params } = this.parseFunctionParams();
            this.eat(TokenType.OpenBrace);
            const body = this.parseBlock();
            rank.methods.push({
                name: name,
                receiver: receiver,
                params: params,
                returnType: typeToken,
                isPub: isPub,
                body: body,
                line: this.currentToken.line,
                column: this.currentToken.column,
                length: 0
            });
        } else {
            // 位域：`uint8 f : 3`（可选位宽，与同类型相邻位域打包）
            let bitWidth: number | undefined;
            if (this.currentToken.type === TokenType.Colon) {
                this.eat(TokenType.Colon);
                bitWidth = Number(this.eat(TokenType.Number).value);
            }
            rank.fields.push({ name: name, type: typeToken, isPub: isPub, ...(bitWidth !== undefined ? { bitWidth } : {}) });
            this.consumeOptionalSemicolon();
        }
    }

    private parseMethod(): FnDecl {
        const returnType = this.parseTypeRef();
        const name = this.eatIdentifierOrKeyword().value;
        const { receiver, params } = this.parseFunctionParams();
        this.eat(TokenType.OpenBrace);
        const body = this.parseBlock();
        return {
            name: name,
            receiver: receiver,
            params: params,
            returnType: returnType,
            isPub: false,
            body: body,
            line: this.currentToken.line,
            column: this.currentToken.column,
            length: 0
        };
    }

    private parseTraitBody(): RankBlock[] {
        this.eat(TokenType.OpenBrace);
        const ranks: RankBlock[] = [];
        const defaultRank: RankBlock = { name: 'classical', fields: [], methods: [] };

        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            if (this.isKeyword('rank')) {
                this.eatKeyword('rank');
                const rankName = this.eatIdentifierOrKeyword().value;
                ranks.push(this.parseTraitRankBody(rankName));
            } else {
                const isPub = this.isKeyword('pub');
                if (isPub) this.eatKeyword('pub');
                this.parseTraitMethod(defaultRank, isPub);
            }
        }
        this.eat(TokenType.CloseBrace);

        if (defaultRank.methods.length > 0) {
            ranks.unshift(defaultRank);
        }
        return ranks;
    }

    private parseTraitRankBody(rankName: string): RankBlock {
        this.eat(TokenType.OpenBrace);
        const block: RankBlock = { name: rankName, fields: [], methods: [] };
        while (this.currentToken.type !== TokenType.CloseBrace && this.currentToken.type !== TokenType.EOF) {
            const isPub = this.isKeyword('pub');
            if (isPub) this.eatKeyword('pub');
            this.parseTraitMethod(block, isPub);
        }
        this.eat(TokenType.CloseBrace);
        return block;
    }

    private parseTraitMethod(rank: RankBlock, isPub: boolean): void {
        const returnType = this.parseTypeRef();
        const name = this.eatIdentifierOrKeyword().value;
        const { receiver, params } = this.parseFunctionParams();
        this.consumeOptionalSemicolon();
        rank.methods.push({
            name: name,
            receiver: receiver,
            params: params,
            returnType: returnType,
            isPub: isPub,
            body: null,
            line: this.currentToken.line,
            column: this.currentToken.column,
            length: 0
        });
    }

    private consumeOptionalSemicolon(): void {
        if (this.currentToken.type === TokenType.Semicolon) {
            this.eat(TokenType.Semicolon);
        }
    }
}