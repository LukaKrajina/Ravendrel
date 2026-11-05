import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { buildMir, MirBody } from './mir';

function build(src: string): MirBody {
    const parser = new Parser(new Lexer(src));
    const ast = parser.parse();
    const prog = buildMir(ast);
    assert.ok(prog.bodies.length >= 1, 'expect at least one body');
    return prog.bodies[0];
}

function count(body: MirBody, kind: string): number {
    return body.blocks.filter(b => b.terminator?.kind === kind).length;
}

test('mir: Qubit local is marked linear, int32 is not', () => {
    const body = build('int32 rvdl_main() { auto q = alloc(); int32 x = 1; return 0; }');
    const q = body.locals.find(l => l.name === 'q');
    const x = body.locals.find(l => l.name === 'x');
    assert.ok(q, 'q local exists');
    assert.ok(x, 'x local exists');
    assert.strictEqual(q.linear, true, 'Qubit is linear');
    assert.strictEqual(x.linear, false, 'int32 is not linear');
});

test('mir: if/else produces a single SwitchInt', () => {
    const body = build('int32 rvdl_main() { int32 x = 1; if (x == 1) { x = 2; } else { x = 3; } return x; }');
    assert.strictEqual(count(body, 'SwitchInt'), 1);
    assert.strictEqual(count(body, 'Return'), 1);
});

test('mir: else-if chain nests as one SwitchInt per if', () => {
    const body = build(
        'int32 rvdl_main() { int32 x = 1; int32 r = 0; ' +
        'if (x == 0) { r = 1; } else if (x == 1) { r = 2; } else { r = 3; } return r; }');
    // 两个 if（外层 + 内层 else-if）各产生一个 SwitchInt
    assert.strictEqual(count(body, 'SwitchInt'), 2);
});

test('mir: logical AND short-circuit produces branching', () => {
    const body = build('int32 rvdl_main() { int32 a = 1; int32 b = 2; int32 c = a < 1 && b > 2; return 0; }');
    // a < 1 的比较用 SwitchInt 实现短回路，故至少 1 个
    assert.ok(count(body, 'SwitchInt') >= 1);
});

test('mir: while produces loop via SwitchInt + backedge Goto', () => {
    const body = build('int32 rvdl_main() { int32 i = 0; while (i < 10) { i = i + 1; } return i; }');
    assert.strictEqual(count(body, 'SwitchInt'), 1);
    assert.ok(count(body, 'Goto') >= 2, 'cond entry + backedge');
});

test('mir: for with break/continue routes to correct targets', () => {
    const body = build(
        'int32 rvdl_main() { int32 i = 0; for (i = 0; i < 10; i = i + 1) { if (i == 5) { continue; } if (i == 8) { break; } } return i; }');
    // 1 个 for 的 SwitchInt + 2 个 if 的 SwitchInt
    assert.ok(count(body, 'SwitchInt') >= 3);
});

test('mir: return produces Return terminator', () => {
    const body = build('int32 rvdl_main() { return 7; }');
    assert.strictEqual(count(body, 'Return'), 1);
});

test('mir: all blocks terminated (well-formed CFG)', () => {
    const body = build(
        'int32 rvdl_main() { int32 x = 1; int32 r = 0; ' +
        'while (x < 3) { x = x + 1; if (x == 2) { r = r + 1; } } return r; }');
    for (const b of body.blocks) {
        assert.notStrictEqual(b.terminator, null, `block ${b.id} must have a terminator`);
    }
});

// ─── 代码生成用 MIR 补全验证 ──────────────────────────────────────
test('mir: fuse lowers to SwitchInt control flow (not opaque Call "fuse")', () => {
    const body = build(
        'int32 rvdl_main() { int32 x = 2; int32 r = fuse (x) { 1: 10, 2: 20, _: 30, }; return r; }');
    assert.ok(count(body, 'SwitchInt') >= 1, 'fuse produces SwitchInt');
    // 不得再出现 "fuse" 占位调用
    const hasFuseCall = body.blocks.some(b =>
        b.statements.some(s => s.kind === 'Assign' && s.rvalue.kind === 'Call' && s.rvalue.target === 'fuse'));
    assert.strictEqual(hasFuseCall, false, 'no opaque fuse call remains');
});

test('mir: builtin call retTy is precise (qchain_balance -> uint64)', () => {
    const body = build('int32 rvdl_main() { uint64 b = qchain_balance("a"); return 0; }');
    const call = body.blocks.flatMap(b => b.statements)
        .find(s => s.kind === 'Assign' && s.rvalue.kind === 'Call' && s.rvalue.target === 'qchain_balance');
    assert.ok(call, 'qchain_balance call exists');
    assert.strictEqual((call as any).rvalue.retTy, 'uint64', 'retTy is uint64, not int32');
});

test('mir: user function call retTy is precise', () => {
    const parser = new Parser(new Lexer(
        'double foo() { return 1.5; } ' +
        'int32 rvdl_main() { double x = foo(); return 0; }'));
    const prog = buildMir(parser.parse());
    const body = prog.bodies.find(b => b.owner === 'rvdl_main')!;
    const call = body.blocks.flatMap(b => b.statements)
        .find(s => s.kind === 'Assign' && s.rvalue.kind === 'Call' && s.rvalue.target === 'foo');
    assert.ok(call, 'foo call exists');
    assert.strictEqual((call as any).rvalue.retTy, 'double', 'retTy is double from user signature');
});

test('mir: lattice index lowers to precise rvdl_lattice_ref', () => {
    const body = build('int32 rvdl_main() { auto b = new lattice<int32, open>(4); int32 v = b[0]; return v; }');
    const call = body.blocks.flatMap(b => b.statements)
        .find(s => s.kind === 'Assign' && s.rvalue.kind === 'Call' && s.rvalue.target === 'rvdl_lattice_ref');
    assert.ok(call, 'rvdl_lattice_ref call exists');
});
