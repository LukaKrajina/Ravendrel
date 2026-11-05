import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { buildTopology } from './topology';
import { IRGenerator } from './ir';

function parse(src: string) {
    return new Parser(new Lexer(src)).parse();
}

function topo(src: string) {
    return buildTopology(parse(src));
}

// ─── @layer 标签解析 ──────────────────────────────────────────────
test('parser: @layer tag is parsed into FunctionDeclaration.layer', () => {
    const ast = parse('@layer(time=0, thread=1, coord=(0,1))\nint32 producer() { return 0; }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.ok(fn.layer, 'expect layer tag');
    assert.strictEqual(fn.layer.time, 0);
    assert.strictEqual(fn.layer.thread, 1);
    assert.deepStrictEqual(fn.layer.coord, [0, 1]);
});

test('parser: @layer with cost/deadline and optional time', () => {
    const ast = parse('@layer(thread=2, coord=(1,2,3), cost=5, deadline=10)\nint32 f() { return 0; }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.strictEqual(fn.layer.time, undefined);       // time 可省略（子函数）
    assert.strictEqual(fn.layer.thread, 2);
    assert.deepStrictEqual(fn.layer.coord, [1, 2, 3]);
    assert.strictEqual(fn.layer.cost, 5);
    assert.strictEqual(fn.layer.deadline, 10);
});

test('parser: @layer coexists with system attribute @[section(...)]', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0))\n@[section(".text.boot")]\nint32 f() { return 0; }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.ok(fn.layer, 'expect layer');
    assert.ok(fn.attributes && fn.attributes.length === 1, 'expect system attribute');
    assert.strictEqual(fn.attributes[0].name, 'section');
});

// ─── 平行 / 叠加自动推导 ──────────────────────────────────────────
test('topology: same coord + adjacent time => stack edge', () => {
    const { topology } = topo(
        '@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }\n' +
        '@layer(time=1, thread=0, coord=(0,0))\nint32 b() { return 0; }');
    assert.ok(topology.edges.some(e => e.kind === 'stack' && e.from === 'a' && e.to === 'b'));
});

test('topology: different coord => parallel edge', () => {
    const { topology } = topo(
        '@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=1, coord=(0,1))\nint32 b() { return 0; }');
    assert.ok(topology.edges.some(e => e.kind === 'parallel' && e.from === 'a' && e.to === 'b'));
});

test('topology: shape aggregates (time, thread, coord)', () => {
    const { topology } = topo(
        '@layer(time=1, thread=3, coord=(0,1))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=0, coord=(2,0))\nint32 b() { return 0; }');
    assert.deepStrictEqual(topology.shape, { time: 2, thread: 4, coord: [3, 2] });
});

// ─── 调用传播延迟（逻辑时钟）──────────────────────────────────────
test('topology: call propagation accumulates deltaT (子函数 = 父 + Δt)', () => {
    const src = [
        '@layer(time=0, thread=0, coord=(1,0), cost=1)',
        'int32 step1() { return 1; }',
        '@layer(time=0, thread=0, coord=(0,0), cost=1)',
        'int32 parent() { int32 a = step1(); int32 b = step1(); return b; }'
    ].join('\n');
    const { topology } = topo(src);
    // 第二次 step1 调用应继承父时钟 τ=2（前面 a=step1 消耗 1 + 变量声明 1）
    const late = topology.callGraph.find(e => e.caller === 'parent' && e.callee === 'step1' && e.deltaT === 2);
    assert.ok(late, 'expect a step1 call at deltaT=2');
    assert.strictEqual(late!.startAt, 2);
});

// ─── 校验 ─────────────────────────────────────────────────────────
test('topology: explicit function without @layer is flagged (E-TOP001)', () => {
    const { errors } = topo('int32 rvdl_main() { return 0; }');
    assert.ok(errors.some(e => e.code === 'E-TOP001'));
});

test('topology: duplicate occupancy is flagged (E-TOP003)', () => {
    const { errors } = topo(
        '@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=0, coord=(0,0))\nint32 b() { return 0; }');
    assert.ok(errors.some(e => e.code === 'E-TOP003'));
});

// ─── 曲面体几何边（projection / inversion / crossing）────────────────
test('topology: coord rank mismatch => projection edge', () => {
    const { topology } = topo(
        '@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=0, coord=(2))\nint32 b() { return 0; }');
    assert.ok(topology.edges.some(e => e.kind === 'projection' && e.from === 'a' && e.to === 'b'));
});

test('topology: same coord + dual mark => inversion edge', () => {
    const { topology } = topo(
        '@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=0, coord=(0,0), dual=1)\nint32 b() { return 0; }');
    assert.ok(topology.edges.some(e => e.kind === 'inversion' && e.from === 'a' && e.to === 'b'));
});

test('topology: same coord + different thread => crossing edge', () => {
    const { topology } = topo(
        '@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=1, coord=(0,0))\nint32 b() { return 0; }');
    assert.ok(topology.edges.some(e => e.kind === 'crossing' && e.from === 'a' && e.to === 'b'));
});

test('parser: @layer dual mark is parsed', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0), dual=1)\nint32 a() { return 0; }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.strictEqual(fn.layer.dual, true);
});

test('topology: deadline violation is flagged (E-TOP006)', () => {
    const { errors } = topo(
        '@layer(time=0, thread=0, coord=(0,0), deadline=1)\n' +
        'int32 slow() { int32 a = 1; int32 b = 2; int32 c = 3; return c; }');
    assert.ok(errors.some(e => e.code === 'E-TOP006'));
});

// ─── 坐标维度：投影合法，但维度差 > 1 需诊断（E-TOP007）───────────────
test('topology: coord dimension spread of 1 is legal (projection edge)', () => {
    // 维度差恰为 1 → 构成 projection 投影边（原 E-TOP002 已放宽为合法），不应报错。
    const { errors } = topo(
        '@layer(time=0, thread=0, coord=(0))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=0, coord=(0,0))\nint32 b() { return 0; }');
    assert.ok(!errors.some(e => e.code === 'E-TOP007'), 'dimension spread of 1 must stay legal');
});

test('topology: coord dimension spread > 1 is flagged (E-TOP007)', () => {
    // 维度差为 2（1 维 vs 3 维）：跨越一个以上维度的投影在几何上无良定义路径。
    const { errors } = topo(
        '@layer(time=0, thread=0, coord=(0))\nint32 a() { return 0; }\n' +
        '@layer(time=0, thread=0, coord=(0,0,0))\nint32 b() { return 0; }');
    assert.ok(errors.some(e => e.code === 'E-TOP007'),
        'expect E-TOP007 for coord dimension spread > 1');
});

test('topology: uniform coord dimensions produce no E-TOP007', () => {
    const { errors } = topo(
        '@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }\n' +
        '@layer(time=1, thread=0, coord=(1,1))\nint32 b() { return 0; }');
    assert.ok(!errors.some(e => e.code === 'E-TOP007'), 'uniform dimensions must not be flagged');
});

// ─── IR 生成：调度表 + 拓扑入口 ───────────────────────────────────
test('ir: @layer functions generate topology entry + dispatch table', () => {
    const ast = parse('@layer(time=0, thread=0, coord=(0,0))\nint32 a() { return 0; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@rvdl_topology_json'), 'expect dispatch table constant');
    assert.ok(ir.includes('define i32 @rvdl_topology_entry'), 'expect topology entry');
    assert.ok(ir.includes('rvdl_runtime_run_topology'), 'expect runtime scheduler ABI');
});
