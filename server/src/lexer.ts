export enum TokenType {
    Let = "Let",
    Function = "Function",
    QlmInvoke = "QlmInvoke",
    EncodeText = "EncodeText",
    EncodeImage = "EncodeImage",
    Allocate = "Allocate",
    Keyword = 'Keyword',
    Identifier = 'Identifier',
    Number = 'Number',
    String = 'String',
    Equals = 'Equals',
    EqualsEquals = 'EqualsEquals',
    OpenBrace = 'OpenBrace',
    OpenParen = "OpenParen",
    OpenBracket = "OpenBracket",
    CloseBrace = 'CloseBrace',
    CloseParen = 'CloseParen',
    CloseBracket = "CloseBracket",
    At = 'At',
    LessThan = 'LessThan',
    LessEqual = 'LessEqual',
    GreaterThan = 'GreaterThan',
    GreaterEqual = 'GreaterEqual',
    Plus = 'Plus',
    Minus = 'Minus',
    Star = 'Star',
    Slash = 'Slash',
    Percent = 'Percent',
    Dot = 'Dot',
    Comma = 'Comma',
    Semicolon = 'Semicolon',
    Colon = 'Colon',
    ColonColon = 'ColonColon',
    Ampersand = 'Ampersand',
    Pipe = 'Pipe',
    Caret = 'Caret',
    Tilde = 'Tilde',
    ShiftLeft = 'ShiftLeft',
    ShiftRight = 'ShiftRight',
    AndAnd = 'AndAnd',
    OrOr = 'OrOr',
    NotEqual = 'NotEqual',
    Bang = 'Bang',
    Arrow = 'Arrow',
    Dollar = 'Dollar',
    Hash = 'Hash',
    EOF = 'EOF'
}

export interface Token {
    type: TokenType;
    value: string;
    line: number;
    column: number;
    length: number;
}

export class Lexer {
    private position: number = 0;
    private line: number = 1;
    private column: number = 1;
    private input: string = '';
    /** 预置 token 缓冲模式：morph 展开后直接喂 token，而非重新扫描源文本 */
    private tokenBuffer: Token[] | null = null;
    private tokenIndex: number = 0;

    constructor(input: string) {
        this.input = input;
    }

    /**
     * 从已展开的 token 数组构造词法器（供 morph.ts 展开后重解析使用）。
     * 缓冲耗尽后返回合成 EOF，不触碰源文本。
     */
    public static fromTokens(tokens: Token[]): Lexer {
        const lexer = new Lexer('');
        lexer.tokenBuffer = tokens;
        return lexer;
    }

    private currentChar(): string {
        return this.position < this.input.length ? this.input[this.position] : '\0';
    }

    private advance(): void {
        if (this.input[this.position] === '\n') {
            this.line++;
            this.column = 1;
        } else {
            this.column++;
        }
        this.position++;
    }

    private skipWhitespaceAndComments(): void {
        while (this.position < this.input.length){
            const ch = this.currentChar();
            if (/\s/.test(ch)){
                this.advance();
            } else if(ch == '/' && this.input[this.position + 1] === '/') {
                while (this.currentChar() != '\n' && this.currentChar() !== '\0'){
                    this.advance();
                }
            } else {
                break;
            }
        }
    }

    public getNextToken(): Token {
        if (this.tokenBuffer) {
            if (this.tokenIndex < this.tokenBuffer.length) {
                return this.tokenBuffer[this.tokenIndex++];
            }
            return { type: TokenType.EOF, value: '', line: 0, column: 0, length: 0 };
        }
        this.skipWhitespaceAndComments();
        const startLine = this.line;
        const startCol = this.column;
        const char = this.currentChar();

        if (char === '\0') {
            return { type: TokenType.EOF, value: '', line: startLine, column: startCol, length: 0 };
        }

        if (char === '=') {
            this.advance();
            if (this.currentChar() === '=') {
                this.advance();
                return { type: TokenType.EqualsEquals, value: '==', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.Equals, value: '=', line: startLine, column: startCol, length: 1 };
        }

        if (char === '!') {
            this.advance();
            if (this.currentChar() === '=') {
                this.advance();
                return { type: TokenType.NotEqual, value: '!=', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.Bang, value: '!', line: startLine, column: startCol, length: 1 };
        }
        
        if (char === '(') { this.advance(); return { type: TokenType.OpenParen, value: '(', line: startLine, column: startCol, length: 1 }; }
        if (char === ')') { this.advance(); return { type: TokenType.CloseParen, value: ')', line: startLine, column: startCol, length: 1 }; }
        if (char === '{') { this.advance(); return { type: TokenType.OpenBrace, value: '{', line: startLine, column: startCol, length: 1 }; }
        if (char === '}') { this.advance(); return { type: TokenType.CloseBrace, value: '}', line: startLine, column: startCol, length: 1 }; }
        if (char === '[') { this.advance(); return { type: TokenType.OpenBracket, value: '[', line: startLine, column: startCol, length: 1 }; }
        if (char === ']') { this.advance(); return { type: TokenType.CloseBracket, value: ']', line: startLine, column: startCol, length: 1 }; }
        if (char === '@') { this.advance(); return { type: TokenType.At, value: '@', line: startLine, column: startCol, length: 1 }; }
        if (char === '$') { this.advance(); return { type: TokenType.Dollar, value: '$', line: startLine, column: startCol, length: 1 }; }
        if (char === '#') { this.advance(); return { type: TokenType.Hash, value: '#', line: startLine, column: startCol, length: 1 }; }
        if (char === '.') { this.advance(); return { type: TokenType.Dot, value: '.', line: startLine, column: startCol, length: 1 }; }
        if (char === ';') { this.advance(); return { type: TokenType.Semicolon, value: ';', line: startLine, column: startCol, length: 1 }; }
        if (char === ',') { this.advance(); return { type: TokenType.Comma, value: ',', line: startLine, column: startCol, length: 1 }; }
        if (char === '<') {
            this.advance();
            if (this.currentChar() === '=') {
                this.advance();
                return { type: TokenType.LessEqual, value: '<=', line: startLine, column: startCol, length: 2 };
            }
            if (this.currentChar() === '<') {
                this.advance();
                return { type: TokenType.ShiftLeft, value: '<<', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.LessThan, value: '<', line: startLine, column: startCol, length: 1 };
        }
        if (char === '+') { this.advance(); return { type: TokenType.Plus, value: '+', line: startLine, column: startCol, length: 1 }; }
        if (char === '-') {
            this.advance();
            if (this.currentChar() === '>') {
                this.advance();
                return { type: TokenType.Arrow, value: '->', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.Minus, value: '-', line: startLine, column: startCol, length: 1 };
        }
        if (char === '*') { this.advance(); return { type: TokenType.Star, value: '*', line: startLine, column: startCol, length: 1 }; }
        if (char === '/') { this.advance(); return { type: TokenType.Slash, value: '/', line: startLine, column: startCol, length: 1 }; }
        if (char === '%') { this.advance(); return { type: TokenType.Percent, value: '%', line: startLine, column: startCol, length: 1 }; }
        if (char === '&') {
            this.advance();
            if (this.currentChar() === '&') {
                this.advance();
                return { type: TokenType.AndAnd, value: '&&', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.Ampersand, value: '&', line: startLine, column: startCol, length: 1 };
        }
        if (char === '|') {
            this.advance();
            if (this.currentChar() === '|') {
                this.advance();
                return { type: TokenType.OrOr, value: '||', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.Pipe, value: '|', line: startLine, column: startCol, length: 1 };
        }
        if (char === '^') { this.advance(); return { type: TokenType.Caret, value: '^', line: startLine, column: startCol, length: 1 }; }
        if (char === '~') { this.advance(); return { type: TokenType.Tilde, value: '~', line: startLine, column: startCol, length: 1 }; }
        if (char === '>') {
            this.advance();
            if (this.currentChar() === '=') {
                this.advance();
                return { type: TokenType.GreaterEqual, value: '>=', line: startLine, column: startCol, length: 2 };
            }
            if (this.currentChar() === '>') {
                this.advance();
                return { type: TokenType.ShiftRight, value: '>>', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.GreaterThan, value: '>', line: startLine, column: startCol, length: 1 };
        }

        if (char === ':') {
            this.advance();
            if (this.currentChar() === ':') {
                this.advance();
                return { type: TokenType.ColonColon, value: '::', line: startLine, column: startCol, length: 2 };
            }
            return { type: TokenType.Colon, value: ':', line: startLine, column: startCol, length: 1 };
        }

        if (char === '"' || char === "'") {
            const quoteType = char;
            let strVal = '';
            this.advance();
            while (this.currentChar() !== quoteType && this.currentChar() !== '\0') {
                if (this.currentChar() === '\\') {
                    // 转义序列:支持常见转义,使字符串字面量能安全容纳任意字符
                    this.advance();
                    const esc = this.currentChar();
                    if (esc === 'n') strVal += '\n';
                    else if (esc === 't') strVal += '\t';
                    else if (esc === 'r') strVal += '\r';
                    else if (esc === '\\') strVal += '\\';
                    else if (esc === '"') strVal += '"';
                    else if (esc === "'") strVal += "'";
                    else if (esc === '0') strVal += '\0';
                    else strVal += esc;
                    this.advance();
                } else {
                    strVal += this.currentChar();
                    this.advance();
                }
            }
            this.advance();
            return {
                type: TokenType.String,
                value: strVal,
                line: startLine,
                column: startCol,
                length: strVal.length + 2
            };
        }

        if (/[0-9]/.test(char)) {
            // 十六进制字面量：0xRRGGBB（用于颜色等）
            if (char === '0' && (this.input[this.position + 1] === 'x' || this.input[this.position + 1] === 'X')) {
                let hexStr = '0x';
                this.advance(); // '0'
                this.advance(); // 'x'
                while (/[0-9a-fA-F]/.test(this.currentChar())) {
                    hexStr += this.currentChar();
                    this.advance();
                }
                return {
                    type: TokenType.Number,
                    value: hexStr,
                    line: startLine,
                    column: startCol,
                    length: hexStr.length
                };
            }
            let numStr = '';
            while (/[0-9\.]/.test(this.currentChar())) {
                numStr += this.currentChar();
                this.advance();
            }
            return {
                type: TokenType.Number,
                value: numStr,
                line: startLine,
                column: startCol,
                length: numStr.length
            };
        }

        if (/[\p{L}_]/u.test(char)) {
            let idStr = '';
            while (/[\p{L}\p{N}_]/u.test(this.currentChar())) {
                idStr += this.currentChar();
                this.advance();
            }

            const keywords = [
                'let', 'auto', 'int', 'new', 'return', 'if', 'else',
                'while', 'int8', 'int16', 'int32', 'int64',
                'uint8', 'uint16', 'uint32', 'uint64',
                'float', 'double', 'complex64', 'complex128', 'string', 'char', 'Qubit', 'QObject', 'QModel',
                'alloc', 'measure', 'encode_text', 'encode_image', 'qlm_invoke',
                'qlm_load', 'rvdl_encode_string', 'qlm_forward', 'rvdl_decode_string',
                'DiracState', 'BellState', 'QuantumRegister',
                // 任意基构建量子态（θ, φ 参数化布洛赫方向）
                'basis_state',
                // 查询 QObject qubit 数（多 qubit 变分 ansatz 判定态维度对齐）
                'qobj_num_qubits',
                'mind_read', 'mind_train', 'mind_feedback',
                'veda_qlm_train',
                // 量子门（h/x/rz/cnot/toffoli/swap/qft/braid/measure_x/measure_y）
                // 已从关键字表移除：它们在语句位置（h(q)）由 parser 的 Identifier
                // 分支识别为函数调用，在表达式位置（x * x）识别为普通标识符。
                'mod', 'use', 'pub', 'form', 'impl', 'trait', 'template', 'rank', 'self', 'for',
                // 晶格数组类型（lattice<T, B>），吸收 Futhark/Remora/Rust 的新范式
                'lattice',
                'break', 'continue',
                'fn',
                'export', 'import', 'extern', 'requires', 'ensures', 'invariant', 'result', 'from',
                'surrogate', 'tanh_quantize', 'lif_step',
                'mellowmax2', 'logsumexp2', 'boltzmann2',
                'tnorm_luk', 'tnorm_prod', 'tnorm_godel',
                'polymer_weight', 'polymer_mix_bound',
                // QCOS syscall ABI + 堆分配
                'make', 'rvdl_sys_call', 'rvdl_sys_calld', 'rvdl_sys_log', 'rvdl_sys_logi',
                // 指针型入口与堆释放（已开放语言绑定，见 semantic.ts / ir.ts）
                'rvdl_sys_callp', 'rvdl_gc_free',
                // QMS 数值内核
                'rvdl_qms_gap', 'rvdl_mix_bound', 'rvdl_qms_conc',
                // QChain 量子区块链内置函数
                'qchain_wallet', 'qchain_mint', 'qchain_transfer', 'qchain_balance',
                'qchain_mine', 'qchain_height', 'qchain_verify',
                'qchain_qkd', 'qchain_qdba', 'qchain_coin_mint', 'qchain_coin_verify',
                // QChain 密码原语 / 抗超时空 / 时空加密
                'qchain_sha3', 'qchain_hmac', 'qchain_hash_unicode',
                'qchain_sign', 'qchain_sign_verify', 'qchain_sign_pubkey',
                'qchain_mlkem_encaps', 'qchain_mlkem_decaps',
                'qchain_causal_verify', 'qchain_cipher_encrypt', 'qchain_cipher_decrypt',
                // 系统级构造（P3 前置：裸机 / 内核编程）
                'cap', 'unsafe', 'null', 'native',
                'sync_load', 'sync_store', 'sync_add', 'sync_cas',
                'sync_lock', 'sync_unlock',
                'outb', 'inb', 'outw', 'inw', 'outl', 'inl',
                'rvdl_gc_alloc', 'addr',
                // 读寄存器 / MSR / CPUID 内建（x86 架构边界：把寄存器读回 rvdl 变量）
                'read_cr0', 'read_cr2', 'read_cr3', 'read_cr4',
                'write_cr0', 'write_cr3', 'invlpg',
                'rdmsr', 'wrmsr', 'cpuid', 'rdtsc', 'read_rflags', 'xgetbv',
                // volatile 内存访问原语（MMIO 轮询在 -O2 下不被优化为死循环）
                'volatile_load', 'volatile_store',
                // 裸汇编块（多指令 / 多寄存器序列）
                'asm',
                // 定长数组类型（静态查找表）
                'arr',
                // 路由分支
                'route', 'path', 'fallback',
                // 自旋循环
                'spin',
                // 固定值
                'fixed',
                // 味
                'flavor',
                // 态射宏（全局、卫生；见 morph.ts）
                'morph',
                // 融合
                'fuse',
                // 并发与纠缠
                'spawn', 'entangle'
            ];

            if (keywords.includes(idStr)) {
                return { type: TokenType.Keyword, value: idStr, line: startLine, column: startCol, length: idStr.length };
            }
            return { type: TokenType.Identifier, value: idStr, line: startLine, column: startCol, length: idStr.length };
        }

        throw new Error(`Lexer Error: Unknown character: ${char}`);
    }
}