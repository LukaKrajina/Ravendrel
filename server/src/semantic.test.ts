import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';

function analyze(src: string): SemanticAnalyzer {
    const parser = new Parser(new Lexer(src));
    const ast = parser.parse();
    const analyzer = new SemanticAnalyzer();
    analyzer.analyze(ast);
    return analyzer;
}

test('semantic: valid program has no errors', () => {
    const a = analyze('int32 x = 1;\nint32 y = x + 1;');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: undefined variable is flagged', () => {
    const a = analyze('auto x = undefined_var;');
    assert.ok(a.errors.some(e => e.message.includes('Undefined variable')));
});

test('semantic: Qubit cannot be cloned (No-Cloning Theorem)', () => {
    const a = analyze('auto q = alloc();\nauto q2 = q;');
    assert.ok(a.errors.some(e => e.message.includes('No-Cloning') || e.message.includes('Cannot copy Qubit')));
});

test('semantic: type mismatch is flagged', () => {
    const a = analyze('int32 x = 1;\nx = "hello";');
    assert.ok(a.errors.some(e => e.message.includes('Type Error')));
});

test('semantic: measure consumes qubit', () => {
    // measure 后 qubit 被消费,再次使用应报错
    const a = analyze('auto q = alloc();\nauto m = measure(q);\nauto m2 = measure(q);');
    assert.ok(a.errors.some(e => e.message.includes('used after measurement')));
});

test('semantic: measure state does not leak across functions', () => {
    // 回归：测量状态必须按函数作用域隔离。
    // 旧实现把 measuredQubits 当成整个文件的集合，导致其它函数里的同名局部
    // 变量被误判为「已测量的 qubit」（QCOS 内核中 let a = ... 触发的假阳性）。
    const src = [
        'int32 probe() {',
        '    let a = alloc();',
        '    h(a);',
        '    let m = measure(a);',
        '    return m;',
        '}',
        'int32 user() {',
        '    let a = 1;',
        '    let b = 2;',
        '    return a + b;',
        '}',
        'int32 entry() { return probe() + user(); }'
    ].join('\n');
    const r = analyze(src);
    assert.strictEqual(r.errors.filter(e => e.message.includes('used after measurement')).length, 0);
});

test('semantic: measure consumes qubit inside the same function', () => {
    // 同函数内重复测量仍必须报错（作用域隔离不能削弱同函数内的约束）
    const src = [
        'int32 twice() {',
        '    let q = alloc();',
        '    let m = measure(q);',
        '    let m2 = measure(q);',
        '    return m + m2;',
        '}',
        'int32 entry() { return twice(); }'
    ].join('\n');
    const r = analyze(src);
    assert.ok(r.errors.some(e => e.message.includes('used after measurement')));
});

test('semantic: while condition must be boolean/numeric', () => {
    const a = analyze('while ("string") { int32 i = 1; }');
    assert.ok(a.errors.some(e => e.message.includes('while condition')));
});

// ─── QChain 量子区块链内置函数 ──────────────────────────────────────
test('semantic: qchain_wallet returns string', () => {
    const a = analyze('string w = qchain_wallet();');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_balance expects string address', () => {
    const a = analyze('uint64 b = qchain_balance("addr");');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_mint with wrong arg count is flagged', () => {
    const a = analyze('qchain_mint();');
    assert.ok(a.errors.some(e => e.message.includes('qchain_mint')));
});

test('semantic: qchain_transfer returns int32', () => {
    const a = analyze('int32 ok = qchain_transfer("a", "b", 100);');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_coin_mint returns QObject, verify takes QObject', () => {
    const a = analyze('QObject c = qchain_coin_mint(8);\nint32 ok = qchain_coin_verify(c);');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_coin_verify with non-QObject is flagged', () => {
    const a = analyze('int32 ok = qchain_coin_verify(42);');
    assert.ok(a.errors.some(e => e.message.includes('qchain_coin_verify')));
});

// ─── QChain 密码原语 / 抗超时空 / 时空加密 ──────────────────────
test('semantic: qchain_sha3 returns string', () => {
    const a = analyze('string h = qchain_sha3("abc");');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_hmac expects 2 args', () => {
    const a = analyze('string h = qchain_hmac("key", "msg");');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_sign and sign_verify', () => {
    const a = analyze('string s = qchain_sign("msg");\nint32 ok = qchain_sign_verify("msg", s);');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_mlkem_encaps returns string', () => {
    const a = analyze('string c = qchain_mlkem_encaps("pk");');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_causal_verify returns int32', () => {
    const a = analyze('int32 ok = qchain_causal_verify();');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: qchain_cipher_encrypt returns string', () => {
    const a = analyze('string c = qchain_cipher_encrypt(12345, "msg");');
    assert.strictEqual(a.errors.length, 0);
});

// ─── 一行多变量定义（VariableDeclarationList）──────────────────────
test('parser: multi-variable declaration produces VariableDeclarationList', () => {
    const parser = new Parser(new Lexer('int32 a = 1, b = 2, c = 3;'));
    const ast = parser.parse();
    const node = ast.body[0] as any;
    assert.strictEqual(node.type, 'VariableDeclarationList');
    assert.deepStrictEqual(node.declarations.map((d: any) => d.identifier), ['a', 'b', 'c']);
});

test('semantic: multi-variable declaration has no errors', () => {
    const a = analyze('int32 a = 1, b = 2, c = 3;');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: multi-variable declaration may reference earlier declarator', () => {
    const a = analyze('int32 a = 1, b = a + 1;');
    assert.strictEqual(a.errors.length, 0);
});

test('semantic: multi-variable declaration type mismatch is flagged', () => {
    const a = analyze('int32 a = 1, b = "hello";');
    assert.ok(a.errors.some(e => e.message.includes('Type Error')));
});