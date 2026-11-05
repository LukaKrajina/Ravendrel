import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer, Token, TokenType } from './lexer';
import { expandMorphTokens } from './morph';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';

/** 词法 + morph 展开，返回展开后的 token 值序列（以空格连接，便于断言） */
function expand(src: string): string {
    const tokens: Token[] = [];
    const lexer = new Lexer(src);
    for (;;) {
        const t = lexer.getNextToken();
        if (t.type === TokenType.EOF) break;
        tokens.push(t);
    }
    return expandMorphTokens(tokens).map(t => t.value).join(' ');
}

/** 端到端：解析 + 语义分析，返回 analyzer（errors 数组可断言） */
function analyze(src: string): SemanticAnalyzer {
    const ast = new Parser(new Lexer(src)).parse();
    const analyzer = new SemanticAnalyzer();
    analyzer.analyze(ast);
    return analyzer;
}

// ─── 表达式宏 ────────────────────────────────────────────────────────────────

test('morph: basic expression macro expands', () => {
    const src = [
        'morph twice($e: expr) { ($e) + ($e) }',
        'int32 rvdl_main() { int32 y = twice!(x * 3); return 0; }'
    ].join('\n');
    const out = expand(src);
    assert.ok(out.includes('( x * 3 ) + ( x * 3 )'), out);
    assert.ok(!out.includes('twice!'), out);
});

test('morph: hygiene renames template-introduced binders (gauge invariance)', () => {
    const src = [
        'morph swap_into($a: expr, $b: expr) { let tmp = $a; $a = $b; $b = tmp }',
        '@layer(time=0, thread=0, coord=(0)) int32 rvdl_main() {',
        '  int32 tmp = 42; int32 x = 1; int32 y = 2;',
        '  swap_into!(x, y);',
        '  return tmp;',
        '}'
    ].join('\n');
    const out = expand(src);
    // 模板引入的 tmp 被 α-重命名（取新原子），使用处的 tmp 保持原样。
    assert.ok(/tmp__m\d+/.test(out), out);
    assert.ok(out.includes('int32 tmp = 42'), out);
    // 模板内部的引用与绑定同名，应一并被重命名（x = y ; y = tmp__mN）
    assert.ok(/y = tmp__m\d+/.test(out), out);
    // 展开后的代码语义无错（无重复变量 / 类型不匹配）
    const a = analyze(src);
    assert.strictEqual(a.errors.length, 0, JSON.stringify(a.errors));
});

// ─── 重复 ────────────────────────────────────────────────────────────────────

test('morph: repetition generates a family of functions', () => {
    const src = [
        'morph make_syscalls($($name: ident),*) {',
        '  $(fn $name() -> int32 { return 0; })*',
        '}',
        'make_syscalls!(do_init, do_tick);'
    ].join('\n');
    const out = expand(src);
    assert.ok(out.includes('fn do_init'), out);
    assert.ok(out.includes('fn do_tick'), out);
    assert.ok(!out.includes('make_syscalls!'), out);
});

// ─── 字符串化 ────────────────────────────────────────────────────────────────

test('morph: stringify (#$name) emits source text', () => {
    const src = [
        'morph tag($n: ident) { rvdl_sys_log(#$n); }',
        'int32 rvdl_main() { tag!(hello); return 0; }'
    ].join('\n');
    const out = expand(src);
    assert.ok(out.includes('rvdl_sys_log ( hello )'), out);
});

// ─── 类型元变量 ──────────────────────────────────────────────────────────────

test('morph: :type captures generic type suffix', () => {
    const src = [
        'morph with_type($t: type) { $t value = 0; }',
        'int32 rvdl_main() { with_type!(cap<int32>); return 0; }'
    ].join('\n');
    const out = expand(src);
    assert.ok(out.includes('cap < int32 > value'), out);
});

// ─── 递归（限深） ────────────────────────────────────────────────────────────

test('morph: chained macros expand (finite recursion)', () => {
    const src = [
        'morph A($e: expr) { B!($e) }',
        'morph B($e: expr) { ($e) + 1 }',
        'int32 rvdl_main() { int32 y = A!(2); return 0; }'
    ].join('\n');
    const out = expand(src);
    assert.ok(out.includes('( 2 ) + 1'), out);
    assert.ok(!out.includes('A!') && !out.includes('B!'), out);
});

test('morph: infinite recursion hits the depth limit', () => {
    const src = 'morph loop($e: expr) { loop!($e) } loop!(1)';
    assert.throws(() => expand(src), /recursion limit/);
});

// ─── 错误诊断 ────────────────────────────────────────────────────────────────

test('morph: undefined morph throws', () => {
    assert.throws(() => expand('int32 rvdl_main() { nope!(1); return 0; }'), /undefined morph/);
});

test('morph: kind mismatch throws (:ident given a literal)', () => {
    const src = 'morph one($e: ident) { $e } one!(123)';
    assert.throws(() => expand(src), /:ident/);
});

test('morph: duplicate morph throws', () => {
    const src = 'morph a($e: expr) { $e } morph a($e: expr) { $e }';
    assert.throws(() => expand(src), /duplicate morph/);
});

test('morph: no-morph program passes through unchanged (fast path)', () => {
    const src = '@layer(time=0, thread=0, coord=(0)) int32 rvdl_main() { int32 x = 1; return x; }';
    const a = analyze(src);
    assert.strictEqual(a.errors.length, 0, JSON.stringify(a.errors));
});

// ─── 全局性 ──────────────────────────────────────────────────────────────────

test('morph: declared at top level and usable across the file', () => {
    const src = [
        'morph inc($x: expr) { ($x) + 1 }',
        'int32 rvdl_main() { int32 a = inc!(1); int32 b = inc!(a); return b; }'
    ].join('\n');
    const out = expand(src);
    assert.strictEqual((out.match(/\+ 1/g) || []).length, 2, out);
});
