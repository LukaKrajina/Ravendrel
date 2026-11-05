import { test } from 'node:test';
import assert from 'node:assert';
import { parseType, typeToString, typeEquals, isAssignable, T, cap, lattice, func } from './types';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';

test('types: parseType maps scalars', () => {
    assert.deepStrictEqual(parseType('int32'), { kind: 'int', width: 32, signed: true });
    assert.deepStrictEqual(parseType('int'), { kind: 'int', width: 32, signed: true });
    assert.deepStrictEqual(parseType('uint64'), { kind: 'int', width: 64, signed: false });
    assert.deepStrictEqual(parseType('double'), { kind: 'float', width: 64 });
    assert.strictEqual(parseType('auto').kind, 'unknown');
});

test('types: parseType maps quantum types', () => {
    assert.strictEqual(parseType('Qubit').kind, 'quantum');
    assert.strictEqual((parseType('Qubit') as any).cls, 'Qubit');
    assert.strictEqual((parseType('QObject') as any).cls, 'QObject');
});

test('types: parseType parses cap<T>', () => {
    const t = parseType('cap<int32>');
    assert.strictEqual(t.kind, 'cap');
    assert.deepStrictEqual((t as any).inner, { kind: 'int', width: 32, signed: true });
});

test('types: parseType parses lattice<T, B>', () => {
    const t = parseType('lattice<int32, open>');
    assert.strictEqual(t.kind, 'lattice');
    assert.strictEqual((t as any).boundary, 'open');
    assert.strictEqual((t as any).elem.kind, 'int');
});

test('types: parseType parses function type', () => {
    const t = parseType('(int32, double)->QObject');
    assert.strictEqual(t.kind, 'func');
    assert.strictEqual((t as any).params.length, 2);
    assert.strictEqual((t as any).ret.kind, 'quantum');
});

test('types: typeToString roundtrips', () => {
    assert.strictEqual(typeToString(parseType('int32')), 'int32');
    assert.strictEqual(typeToString(parseType('cap<int32>')), 'cap<int32>');
    assert.strictEqual(typeToString(parseType('lattice<int32, open>')), 'lattice<int32, open>');
});

test('types: typeEquals structural equality', () => {
    assert.ok(typeEquals(T.int32, T.int32));
    assert.ok(!typeEquals(T.int32, T.int64));
    assert.ok(!typeEquals(T.int32, T.uint32));
    assert.ok(typeEquals(T.unknown, T.int32), 'unknown is wildcard');
    assert.ok(typeEquals(cap(T.int32), cap(T.int32)));
    assert.ok(typeEquals(func([T.int32], T.double), func([T.int32], T.double)));
});

test('types: isAssignable numeric coercion + null->cap', () => {
    assert.ok(isAssignable(T.double, T.int32), 'int32 -> double coerces');
    assert.ok(isAssignable(T.int32, T.int32));
    assert.ok(!isAssignable(T.int32, T.string));
    assert.ok(isAssignable(cap(T.int32), T.null), 'null -> cap');
    assert.ok(isAssignable(cap(T.int32), T.int32), 'int -> cap (MMIO)');
});

test('semantic: user function signature is checked', () => {
    const src = [
        '@layer(time=0, thread=0, coord=(0))',
        'int32 add(int32 a, int32 b) { return a + b; }',
        '@layer(time=0, thread=0, coord=(1))',
        'int32 main() { int32 x = add(1, 2); int32 y = add("wrong", 2); return x; }'
    ].join('\n');
    const ast = new Parser(new Lexer(src)).parse();
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes("argument 1 of 'add'")), 'wrong arg type is flagged');
});

test('semantic: user function arg count mismatch is flagged', () => {
    const src = [
        '@layer(time=0, thread=0, coord=(0))',
        'int32 add(int32 a, int32 b) { return a + b; }',
        '@layer(time=0, thread=0, coord=(1))',
        'int32 main() { int32 x = add(1); return x; }'
    ].join('\n');
    const ast = new Parser(new Lexer(src)).parse();
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes("expects 2 argument(s), got 1")), 'arg count mismatch is flagged');
});
