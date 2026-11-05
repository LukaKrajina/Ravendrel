import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer, TokenType } from './lexer';

function tokenize(src: string) {
    const lexer = new Lexer(src);
    const tokens: { type: TokenType; value: string; line: number; column: number }[] = [];
    for (;;) {
        const t = lexer.getNextToken();
        tokens.push(t);
        if (t.type === TokenType.EOF) break;
    }
    return tokens;
}

test('lexer: keywords and identifiers', () => {
    const tokens = tokenize('let x = 5;');
    assert.strictEqual(tokens[0].type, TokenType.Keyword);
    assert.strictEqual(tokens[0].value, 'let');
    assert.strictEqual(tokens[1].type, TokenType.Identifier);
    assert.strictEqual(tokens[1].value, 'x');
    assert.strictEqual(tokens[2].type, TokenType.Equals);
});

test('lexer: numbers (int and float)', () => {
    const tokens = tokenize('3 3.14');
    assert.strictEqual(tokens[0].type, TokenType.Number);
    assert.strictEqual(tokens[0].value, '3');
    assert.strictEqual(tokens[1].type, TokenType.Number);
    assert.strictEqual(tokens[1].value, '3.14');
});

test('lexer: operators', () => {
    const tokens = tokenize('== != <= >= && || ->');
    assert.strictEqual(tokens[0].type, TokenType.EqualsEquals);
    assert.strictEqual(tokens[1].type, TokenType.NotEqual);
    assert.strictEqual(tokens[2].type, TokenType.LessEqual);
    assert.strictEqual(tokens[3].type, TokenType.GreaterEqual);
    assert.strictEqual(tokens[4].type, TokenType.AndAnd);
    assert.strictEqual(tokens[5].type, TokenType.OrOr);
    assert.strictEqual(tokens[6].type, TokenType.Arrow);
});

test('lexer: string with escape sequences', () => {
    const tokens = tokenize('"a\\"b\\\\c\\nd"');
    assert.strictEqual(tokens[0].type, TokenType.String);
    // 转义后应为 a"b\c 换行 d
    assert.strictEqual(tokens[0].value, 'a"b\\c\nd');
});

test('lexer: line/column tracking', () => {
    const tokens = tokenize('let x = 1;\nlet y = 2;');
    // 第二个 let 在第 2 行
    assert.strictEqual(tokens[5].line, 2);
});

test('lexer: throws on unknown character', () => {
    // `#` 现为 morph 宏的字符串化 token，改用仍不受支持的 `` ` ``（反引号）。
    assert.throws(() => tokenize('`'));
});