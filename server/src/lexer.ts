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
    private tokenBuffer: Token[] | null = null;
    private tokenIndex: number = 0;

    constructor(input: string) {
        this.input = input;
    }




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
            if (char === '0' && (this.input[this.position + 1] === 'x' || this.input[this.position + 1] === 'X')) {
                let hexStr = '0x';
                this.advance();
                this.advance();
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
                'alloc', 'measure', 'encode_text', 'encode_image', 'encode_pixel', 'qlm_invoke',
                'qlm_load', 'rvdl_encode_string', 'qlm_forward', 'rvdl_decode_string',
                'DiracState', 'BellState', 'QuantumRegister',
                'basis_state',
                'qobj_num_qubits',
                'mind_read', 'mind_train', 'mind_feedback',
                'veda_qlm_train',
                'mod', 'use', 'pub', 'form', 'impl', 'trait', 'template', 'rank', 'self', 'for',
                'lattice',
                'break', 'continue',
                'fn',
                'export', 'import', 'extern', 'requires', 'ensures', 'invariant', 'result', 'from',
                'surrogate', 'tanh_quantize', 'lif_step',
                'mellowmax2', 'logsumexp2', 'boltzmann2',
                'tnorm_luk', 'tnorm_prod', 'tnorm_godel',
                'polymer_weight', 'polymer_mix_bound',
                'make', 'rvdl_sys_call', 'rvdl_sys_calld', 'rvdl_sys_log', 'rvdl_sys_logi',
                'rvdl_sys_callp', 'rvdl_gc_free',
                'rvdl_qms_gap', 'rvdl_mix_bound', 'rvdl_qms_conc',
                'qchain_wallet', 'qchain_mint', 'qchain_transfer', 'qchain_balance',
                'qchain_mine', 'qchain_height', 'qchain_verify',
                'qchain_qkd', 'qchain_qdba', 'qchain_coin_mint', 'qchain_coin_verify',
                'qchain_sha3', 'qchain_hmac', 'qchain_hash_unicode',
                'qchain_sign', 'qchain_sign_verify', 'qchain_sign_pubkey',
                'qchain_mlkem_encaps', 'qchain_mlkem_decaps',
                'qchain_causal_verify', 'qchain_cipher_encrypt', 'qchain_cipher_decrypt',
                'cap', 'unsafe', 'null', 'native',
                'sync_load', 'sync_store', 'sync_add', 'sync_cas',
                'sync_lock', 'sync_unlock',
                'outb', 'inb', 'outw', 'inw', 'outl', 'inl',
                'rvdl_gc_alloc', 'addr',
                'read_cr0', 'read_cr2', 'read_cr3', 'read_cr4',
                'write_cr0', 'write_cr3', 'invlpg',
                'rdmsr', 'wrmsr', 'cpuid', 'rdtsc', 'read_rflags', 'xgetbv',
                'volatile_load', 'volatile_store',
                'asm',
                'arr',
                'route', 'path', 'fallback',
                'spin',
                'fixed',
                'flavor',
                'morph',
                'fuse',
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