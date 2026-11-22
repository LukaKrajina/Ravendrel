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






export const LAYER_TAG_SCALAR_KEYS: ReadonlySet<string> = new Set([
    'time', 'thread', 'cost', 'deadline', 'dual',
    'shuttle_lambda', 'shuttle_phase', 'shuttle_rounds', 'shuttle_strategy',
    'track_mode', 'track_slots',
]);




export const LAYER_TAG_LIST_KEYS: ReadonlySet<string> = new Set(['coord', 'shuttle', 'track']);

const QUANTUM_ATTR_NAMES = new Set(['gate', 'undo', 'steer', 'unitary', 'measure']);




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

const PHYSICAL_ATTR_NAMES = new Set(['coherence', 'noise', 'basis', 'decoherence_free', 'error_correction']);




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
    'alloc', 'measure', 'basis_state', 'encode_text', 'encode_image', 'encode_pixel', 'encode_amplitudes', 'qobj_num_qubits', 'qlm_invoke',
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

    private drainTokens(): Token[] {
        const tokens: Token[] = [];
        while (this.currentToken.type !== TokenType.EOF) {
            tokens.push(this.advance());
        }
        return tokens;
    }




    private parseAttributes(): FunctionAttribute[] {
        const attrs: FunctionAttribute[] = [];
        while (this.currentToken.type === TokenType.At && this.peek().type === TokenType.OpenBracket) {
            this.eat(TokenType.At);
            this.eat(TokenType.OpenBracket);
            const name = this.eatIdentifierOrKeyword().value;
            let value: string | null = null;
            let nums: number[] | undefined;
            const t = this.currentToken.type as TokenType;
            if (t === TokenType.OpenParen) {
                this.eat(TokenType.OpenParen);
                const first = this.currentToken.type as TokenType;
                if (first === TokenType.String) {
                    value = this.eat(TokenType.String).value;
                } else {
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















    private parseLayerTag(): LayerTag {
        this.eat(TokenType.At);
        this.eatIdentifierOrKeyword();
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
                const sh = (tag.shuttle = tag.shuttle ?? { targets: [] });
                sh.targets.push(...this.parseLayerCoordGroups());
            } else if (key === 'track') {
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
                else if (key === 'dual') tag.dual = (v !== 0);
                else if (key === 'shuttle_lambda') (tag.shuttle = tag.shuttle ?? { targets: [] }).lambda = v;
                else if (key === 'shuttle_phase') (tag.shuttle = tag.shuttle ?? { targets: [] }).phase = v;
                else if (key === 'shuttle_rounds') (tag.shuttle = tag.shuttle ?? { targets: [] }).rounds = v;
                else if (key === 'shuttle_strategy') (tag.shuttle = tag.shuttle ?? { targets: [] }).strategy = v as ShuttleStrategyValue;
                else if (key === 'track_mode') (tag.track = tag.track ?? { observables: [] }).mode = v;
                else if (key === 'track_slots') (tag.track = tag.track ?? { observables: [] }).slots = v;
            } else {
                unknown.push({ key, line: keyTok.line, column: keyTok.column });
                this.skipLayerValue();
            }
            if (this.currentToken.type === TokenType.Comma) this.eat(TokenType.Comma);
        }
        this.eat(TokenType.CloseParen);

        if (unknown.length > 0) tag.unknownKeys = unknown;
        return tag;
    }











    private parseLayerSignedNumber(): number {
        let sign = 1;
        if ((this.currentToken.type as TokenType) === TokenType.Minus) {
            this.eat(TokenType.Minus);
            sign = -1;
        }
        return sign * Number(this.eat(TokenType.Number).value);
    }







    private parseLayerScalarValue(): number {
        const t = this.currentToken.type as TokenType;
        if (t === TokenType.Identifier || t === TokenType.Keyword) {
            const word = this.currentToken.value;
            if (word === 'true') { this.advance(); return 1; }
            if (word === 'false') { this.advance(); return 0; }
        }
        return this.parseLayerSignedNumber();
    }




    private parseLayerNumberList(): number[] {
        this.eat(TokenType.OpenParen);
        const out = this.parseLayerNumberListBody();
        this.eat(TokenType.CloseParen);
        return out;
    }

    private parseLayerNumberListBody(): number[] {
        const out: number[] = [];
        while ((this.currentToken.type as TokenType) !== TokenType.CloseParen && (this.currentToken.type as TokenType) !== TokenType.EOF) {
            out.push(this.parseLayerSignedNumber());
            if ((this.currentToken.type as TokenType) === TokenType.Comma) this.eat(TokenType.Comma);
        }
        return out;
    }














    private parseLayerCoordGroups(): number[][] {
        if ((this.currentToken.type as TokenType) !== TokenType.OpenParen) {
            return [[this.parseLayerSignedNumber()]];
        }
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

            let isPub = false;
            let isExport = false;
            if (this.isKeyword('pub')) {
                this.eatKeyword('pub');
                isPub = true;
            } else if (this.isKeyword('export')) {
                this.eatKeyword('export');
                isExport = true;
            }
            const fn = this.parseDeclarationOrFunction(isPub, isExport) as FunctionDeclaration;
            fn.layer = layer;
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
        let isFixed = false;
        if (varType === 'fixed') {
            isFixed = true;
            varType = this.eatIdentifierOrKeyword().value;
        }
        if (varType === 'let' || varType === 'auto') varType = 'auto';

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
            if (opToken.type === TokenType.Star) {
                return {
                    type: 'Dereference',
                    target: argument,
                    line: opToken.line,
                    column: opToken.column,
                    length: argument.column + argument.length - opToken.column
                } as Dereference;
            }
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
        if ((result === 'cap' || result === 'lattice') && this.currentToken.type === TokenType.LessThan) {
            result = this.parseGenericTypeRef(result);
        } else if (result === 'arr' && this.currentToken.type === TokenType.LessThan) {
            result = this.parseArrTypeRef();
        } else if (result === 'fn' && this.currentToken.type === TokenType.LessThan) {
            result = this.parseFnTypeRef();
        }
        return result;
    }

    private parseArrTypeRef(): string {
        this.eat(TokenType.LessThan);
        const elem = this.parseTypeRef();
        this.eat(TokenType.Comma);
        const len = this.eat(TokenType.Number).value;
        this.eat(TokenType.GreaterThan);
        return `arr<${elem}, ${len}>`;
    }

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