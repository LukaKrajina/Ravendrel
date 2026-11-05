import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { IRGenerator } from './ir';
import { SemanticAnalyzer } from './semantic';
import { buildMir } from './mir';

function parse(src: string) {
    return new Parser(new Lexer(src)).parse();
}

test('syslevel: cap<int32> maps to i32*', () => {
    const ast = parse('int32 rvdl_main() { unsafe { cap<int32> p = null; } return 0; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('alloca i32*'), 'cap<int32> allocates i32*');
    assert.ok(ir.includes('i32* null'), 'null stored as i32*');
});

test('syslevel: dereference generates load and store', () => {
    const ast = parse('int32 rvdl_main() { unsafe { cap<int32> p = null; *p = 5; int32 x = *p; } return 0; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('store i32 5'), 'store through capability');
    assert.ok(ir.includes('load i32, i32*'), 'load through capability');
});

test('syslevel: semantic accepts dereference', () => {
    const a = new SemanticAnalyzer();
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { unsafe { cap<int32> p = null; int32 x = *p; } return 0; }');
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, []);
});

test('syslevel: MIR lowering handles cap + deref', () => {
    const ast = parse('int32 rvdl_main() { unsafe { cap<int32> p = null; int32 x = *p; } return 0; }');
    const prog = buildMir(ast);
    assert.ok(prog.bodies.length >= 1);
});

test('syslevel: cap return type lowers to i32*', () => {
    const ast = parse('cap<int32> rvdl_main() { return null; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('define i32* @rvdl_main'));
});

test('syslevel: native emits sideeffect inline asm', () => {
    const ast = parse('int32 rvdl_main() { unsafe { native("cli"); } return 0; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('call void asm sideeffect "cli"'));
});

test('syslevel: sync builtins emit atomic IR', () => {
    const ast = parse('int32 rvdl_main() { unsafe { cap<int32> p = null; sync_store(p, 1); int32 x = sync_load(p); int32 y = sync_add(p, 1); int32 z = sync_cas(p, 1, 2); } return 0; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('store atomic i32'), 'sync_store');
    assert.ok(ir.includes('load atomic i32'), 'sync_load');
    assert.ok(ir.includes('atomicrmw add'), 'sync_add');
    assert.ok(ir.includes('cmpxchg'), 'sync_cas');
});

test('syslevel: function attributes emit place/raw', () => {
    const ast = parse('@[place(".text.boot")] @[raw] int32 rvdl_main() { return 0; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('section ".text.boot"'), 'place attribute');
    assert.ok(ir.includes('naked'), 'raw attribute');
});

test('syslevel: bitwise operators emit or/and/shl/ashr/xor', () => {
    const ast = parse('int32 rvdl_main() { int32 a = 1 | 2; int32 b = a & 1; int32 c = 1 << 3; int32 d = c >> 1; int32 e = 5 ^ 3; int32 f = ~0; return a + b + c + d + e + f; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes(' or '), 'bitwise or');
    assert.ok(ir.includes(' and '), 'bitwise and');
    assert.ok(ir.includes(' shl '), 'shift left');
    assert.ok(ir.includes(' ashr '), 'shift right');
    assert.ok(ir.includes(' xor '), 'xor');
});

test('syslevel: cap pointer arithmetic emits getelementptr', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { unsafe { cap<int32> p = null; cap<int32> q = p + 1; } return 0; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'pointer arithmetic is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('getelementptr'), 'pointer arithmetic lowers to gep');
});

test('syslevel: int -> cap emits inttoptr', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { unsafe { cap<int32> p = 753664; int32 x = *p; } return 0; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'int-to-cap is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('inttoptr'), 'int-to-cap lowers to inttoptr');
});

test('syslevel: address-of emits function symbol', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0,0))\nint32 my_handler() { return 1; }\n@layer(time=0, thread=0, coord=(1,0))\nint32 rvdl_main() { unsafe { cap<uint8> h = &my_handler; } return 0; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'address-of is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@my_handler'), 'address-of references function');
});

test('syslevel: address-of local variable emits alloca pointer', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { int32 x = 42; unsafe { cap<int32> p = &x; int32 y = *p; } return y; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'address-of local is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('alloca i32*'), 'cap<int32> allocates i32* slot for the borrow');
    assert.ok(!ir.includes('@x'), 'local is not treated as a function symbol');
});

test('syslevel: rvdl_gc_free and rvdl_sys_callp emit calls', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { unsafe { cap<int32> p = rvdl_gc_alloc(16); rvdl_gc_free(p); cap<uint8> m = rvdl_sys_callp(4, 4096, 0, 0); } return 0; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'rvdl_gc_free/rvdl_sys_callp are type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@rvdl_gc_free'), 'rvdl_gc_free emits call');
    assert.ok(ir.includes('@rvdl_sys_callp'), 'rvdl_sys_callp emits call');
});

test('syslevel: route statement generates comparisons and branches', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { int32 x = 2; int32 r = 0; route (x) { path 1: { r = 10; } path 2: { r = 20; } fallback: { r = 30; } } return r; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'route is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('icmp eq'), 'route generates comparisons');
    assert.ok(ir.includes('route_path_'), 'route generates path labels');
});

test('syslevel: entry function can return any definite type', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\ndouble rvdl_main() { return 1.5; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'double return type is accepted');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('define double @rvdl_main'), 'double return type lowers to double');
});

test('syslevel: entry function rejects unknown return type', () => {
    const ast = parse('auto rvdl_main() { return 1; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('definite type')), 'auto return type is rejected');
});

test('syslevel: spin executes body before checking condition', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { int32 i = 0; spin { i = i + 1; } while (i < 3); return i; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'spin is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('spin_body_'), 'spin generates body label');
    assert.ok(ir.includes('spin_cond_'), 'spin generates condition label');
});

test('syslevel: fixed value cannot be reassigned', () => {
    const ast = parse('int32 rvdl_main() { fixed int32 X = 5; X = 6; return X; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('cannot reassign fixed')), 'fixed reassignment is rejected');
});

test('syslevel: flavor members resolve to integers', () => {
    const ast = parse('flavor Color { RED, GREEN, BLUE }\n@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { int32 x = GREEN; int32 y = BLUE; return x + y; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'flavor is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('rvdl_main'), 'flavor declaration does not break codegen');
});

test('syslevel: fuse pattern matching returns matched arm', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { int32 x = 2; int32 r = fuse (x) { 1: 10, 2: 20, _: 30, }; return r; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'fuse is type-safe');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('fuse_cmp_'), 'fuse generates comparison labels');
    assert.ok(ir.includes('fuse_arm_'), 'fuse generates arm labels');
});

test('syslevel: fuse arms must have same type', () => {
    const ast = parse('int32 rvdl_main() { int32 x = 1; int32 r = fuse (x) { 1: 10, _: 2.5, }; return r; }');
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('same type')), 'fuse arm type mismatch is rejected');
});
