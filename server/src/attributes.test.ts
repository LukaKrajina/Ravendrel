import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';
import { IRGenerator } from './ir';
import { buildMir } from './mir';
import { synthesizeGates } from './gate-synth';

function parse(src: string) {
    return new Parser(new Lexer(src)).parse();
}

// ─── parser：量子门属性解析 ─────────────────────────────────────────
test('parser: @[gate] parsed into quantum.isGate and separated from system attrs', () => {
    const ast = parse('@[gate]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.ok(fn.quantum, 'expect quantum');
    assert.strictEqual(fn.quantum.isGate, true);
    assert.strictEqual(fn.quantum.undo, false);
    // 量子属性不应残留在系统属性列表里
    assert.ok(!fn.attributes || fn.attributes.length === 0, 'gate must not be a system attribute');
});

test('parser: @[undo] @[steer] @[unitary] @[measure] combined', () => {
    const ast = parse('@[undo] @[steer] @[unitary]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.strictEqual(fn.quantum.undo, true);
    assert.strictEqual(fn.quantum.steer, true);
    assert.strictEqual(fn.quantum.unitary, true);
    assert.strictEqual(fn.quantum.isGate, false);
    assert.strictEqual(fn.quantum.measure, false);
});

test('parser: @[gate] coexists with system attribute @[inline]', () => {
    const ast = parse('@[gate] @[inline]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.strictEqual(fn.quantum.isGate, true);
    assert.strictEqual(fn.attributes.length, 1);
    assert.strictEqual(fn.attributes[0].name, 'inline');
});

// ─── ir：经典编译属性映射 + section/naked 命名统一 ─────────────────
test('ir: classic attributes map to LLVM function attributes', () => {
    const ast = parse('@[inline] @[pure] @[cold] @[export]\n@layer(time=0, thread=0, coord=(0))\nint32 f() { return 1; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('alwaysinline'), 'inline -> alwaysinline');
    assert.ok(ir.includes('readnone'), 'pure -> readnone');
    assert.ok(ir.includes('cold'), 'cold');
    assert.ok(ir.includes('dllexport'), 'export -> dllexport');
});

test('ir: @[noinline] @[noreturn] @[readonly] map correctly', () => {
    const ast = parse('@[noinline] @[noreturn]\n@layer(time=0, thread=0, coord=(0))\nint32 f() { return 1; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('noinline'), 'noinline');
    assert.ok(ir.includes('noreturn'), 'noreturn');
});

test('ir: @[section] and @[naked] are unified (not place/raw only)', () => {
    const ast = parse('@[section(".text.boot")] @[naked]\n@layer(time=0, thread=0, coord=(0))\nint32 f() { return 0; }');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('section ".text.boot"'), 'section');
    assert.ok(ir.includes('naked'), 'naked');
});

test('ir: @[gate] custom gate emits define + call (executable)', () => {
    const src = '@[gate]\nvoid my_gate(Qubit q) { h(q); }\n@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { Qubit q = alloc(); my_gate(q); return 0; }';
    const ast = parse(src);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('define void @my_gate'), 'expect define @my_gate');
    assert.ok(ir.includes('call void @my_gate'), 'expect call @my_gate');
});

test('ir: spawn generates thread function + rvdl_spawn call (real concurrency)', () => {
    const src = '@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { spawn { int32 x = 1; } return 0; }';
    const ast = parse(src);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('define void @rvdl_thread_1'), 'expect thread function');
    assert.ok(ir.includes('call void @rvdl_spawn'), 'expect rvdl_spawn call');
});

test('ir: spawn captures outer variable via env closure', () => {
    const src = '@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { int32 x = 42; spawn { int32 y = x; } return 0; }';
    const ast = parse(src);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('define void @rvdl_thread_1(i8* %env)'), 'expect thread function with env param');
    assert.ok(ir.includes('call void @rvdl_spawn'), 'expect rvdl_spawn call');
    assert.ok(ir.includes('getelementptr'), 'expect env field access');
});

test('mir: lambda is lowered (no borrow check skip)', () => {
    const src = '@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { auto f = fn(int32 a) -> int32 { return a + 1; }; return 0; }';
    const ast = parse(src);
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.ok(!a.errors.some(e => e.message.includes('unsupported expression')), 'lambda should lower without error');
});

test('mir: form fields are serialized (for Member getelementptr)', () => {
    const src = 'form Point { double x; double y; }\n@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { return 0; }';
    const ast = parse(src);
    const mir = buildMir(ast);
    assert.ok(mir.forms, 'expect forms');
    const point = mir.forms!.find(f => f.name === 'Point');
    assert.ok(point, 'expect Point form');
    assert.deepStrictEqual(point!.fields.map(f => f.name), ['x', 'y']);
});

// ─── semantic：量子属性校验 ────────────────────────────────────────
test('semantic: @[unitary] must not measure', () => {
    const a = new SemanticAnalyzer();
    const ast = parse('@[unitary]\n@layer(time=0, thread=0, coord=(0))\nint32 f(Qubit q) { return measure(q); }');
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('E-QUNI')), 'expect E-QUNI');
});

test('semantic: @[gate] must not measure', () => {
    const a = new SemanticAnalyzer();
    const ast = parse('@[gate]\n@layer(time=0, thread=0, coord=(0))\nint32 f(Qubit q) { return measure(q); }');
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('E-QUNI')), 'expect E-QUNI for @[gate]');
});

test('semantic: @[undo] requires @[gate] or @[unitary]', () => {
    const a = new SemanticAnalyzer();
    const ast = parse('@[undo]\n@layer(time=0, thread=0, coord=(0))\nint32 f(Qubit q) { return 0; }');
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('E-QSYN')), 'expect E-QSYN');
});

test('semantic: @[gate] function with only gates is accepted (no E-QUNI)', () => {
    const a = new SemanticAnalyzer();
    const ast = parse('@[gate]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); x(q); }');
    a.analyze(ast);
    assert.ok(!a.errors.some(e => e.message.includes('E-QUNI')), 'gate-only function must not raise E-QUNI');
});

// ─── mir：@[gate] 函数识别为门 ─────────────────────────────────────
test('mir: @[gate] function call is lowered to QGate', () => {
    const ast = parse('@[gate]\nvoid my_gate(Qubit q) { h(q); }\n@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { Qubit q = alloc(); my_gate(q); return 0; }');
    const mir = buildMir(ast);
    let found = false;
    for (const b of mir.bodies) {
        for (const bb of b.blocks) {
            for (const s of bb.statements) {
                const rv = (s as any).rvalue;
                if (rv && rv.kind === 'QGate' && rv.gate === 'my_gate') found = true;
            }
        }
    }
    assert.ok(found, 'expect QGate(gate="my_gate") for @[gate] function');
});

// ─── gate-synth：@[undo] / @[steer] 门合成（可逆编织范式）──────────
test('gate-synth: @[undo] synthesizes reversible dual (reverse + dagger)', () => {
    const ast = parse('@[gate] @[undo]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { h(q); rz(q, 0.5); }');
    synthesizeGates(ast);
    const undo = ast.body.find(n => (n as any).name === 'U_undo') as any;
    assert.ok(undo, 'expect U_undo');
    assert.strictEqual(undo.synthetic, true, 'synthetic marker');
    // 门序反转：原 h; rz → 反转为 rz†; h†，即 rz 在前、h 在后
    const gateNames = undo.body.map((s: any) => s.expression.name);
    assert.deepStrictEqual(gateNames, ['rz', 'h'], 'reversed gate order');
    // rz 取逆：θ → -θ（UnaryExpression '-'，取负的是角度而非 qubit）
    const rzAngle = undo.body[0].expression.arguments[1];
    assert.strictEqual(rzAngle.type, 'UnaryExpression');
    assert.strictEqual(rzAngle.operator, '-');
    assert.strictEqual(rzAngle.argument.type, 'NumberLiteral', 'negation must target the angle, not the qubit');
    assert.strictEqual(rzAngle.argument.value, 0.5);
    // qubit 参数不被取逆修饰
    assert.strictEqual(undo.body[0].expression.arguments[0].name, 'q');
});

test('gate-synth: @[undo] of s expands to rz(-π/2)', () => {
    const ast = parse('@[gate] @[undo]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { s(q); }');
    synthesizeGates(ast);
    const undo = ast.body.find(n => (n as any).name === 'U_undo') as any;
    assert.ok(undo, 'expect U_undo');
    assert.strictEqual(undo.body[0].expression.name, 'rz', 'S† = Rz(-π/2)');
    const ang = undo.body[0].expression.arguments[1];
    assert.strictEqual(ang.type, 'NumberLiteral');
    assert.ok(Math.abs(ang.value + Math.PI / 2) < 1e-12, 'angle = -π/2');
});

test('gate-synth: @[undo] of t expands to rz(-π/4)', () => {
    const ast = parse('@[gate] @[undo]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { t(q); }');
    synthesizeGates(ast);
    const undo = ast.body.find(n => (n as any).name === 'U_undo') as any;
    assert.ok(undo, 'expect U_undo');
    assert.strictEqual(undo.body[0].expression.name, 'rz', 'T† = Rz(-π/4)');
    const ang = undo.body[0].expression.arguments[1];
    assert.ok(Math.abs(ang.value + Math.PI / 4) < 1e-12, 'angle = -π/4');
});

test('gate-synth: @[steer] synthesizes coherent control (ctrl + steered gates)', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { x(q); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    assert.strictEqual(steer.synthetic, true);
    // 额外控制位
    assert.strictEqual(steer.params[0].name, '__ctrl');
    assert.strictEqual(steer.params[0].type, 'Qubit');
    // x → cx（受控门名映射）
    assert.strictEqual(steer.body[0].expression.name, 'cx');
});

test('gate-synth: user gate dagger recurses into <name>_undo', () => {
    const ast = parse('@[gate]\nvoid G(Qubit q) { h(q); }\n@[gate] @[undo]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { G(q); }');
    synthesizeGates(ast);
    const undo = ast.body.find(n => (n as any).name === 'U_undo') as any;
    assert.ok(undo, 'expect U_undo');
    // 用户门 G 的可逆对偶是 G_undo
    assert.strictEqual(undo.body[0].expression.name, 'G_undo');
});

test('gate-synth: qft dagger is iqft (not self-inverse approximation)', () => {
    const ast = parse('@[gate] @[undo]\n@layer(time=0, thread=0, coord=(0))\nvoid U() { qft(3); }');
    synthesizeGates(ast);
    const undo = ast.body.find(n => (n as any).name === 'U_undo') as any;
    assert.ok(undo, 'expect U_undo');
    assert.strictEqual(undo.body[0].expression.name, 'iqft', 'qft† = iqft');
});

test('gate-synth: control flow in @[undo] is rejected (E-QCTRL)', () => {
    const warnings = synthesizeGates(parse(
        '@[gate] @[undo]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { if (true) { h(q); } }'));
    assert.ok(warnings.some(w => w.includes('E-QCTRL')), 'expect E-QCTRL warning');
});

test('gate-synth: qft steer is cqft', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U() { qft(3); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    assert.strictEqual(steer.body[0].expression.name, 'cqft', 'qft steer = cqft');
    assert.strictEqual(steer.body[0].expression.arguments[0].name, '__ctrl', 'ctrl param');
});

test('gate-synth: braid steer is cbraid', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit a, Qubit b) { braid(a, b); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    assert.strictEqual(steer.body[0].expression.name, 'cbraid', 'braid steer = cbraid');
});

test('gate-synth: toffoli steer is c_toffoli with extra control', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit a, Qubit b, Qubit t) { toffoli(a, b, t); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    assert.strictEqual(steer.body[0].expression.name, 'c_toffoli');
    const args = steer.body[0].expression.arguments;
    assert.strictEqual(args[0].name, '__ctrl', 'extra control first');
    assert.deepStrictEqual(args.slice(1).map((a: any) => a.name), ['a', 'b', 't']);
});

test('gate-synth: z steer decomposes to [h, cnot, h]', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { z(q); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    const names = steer.body.map((s: any) => s.expression.name);
    assert.deepStrictEqual(names, ['h', 'cnot', 'h'], 'z steer = H·CNOT·H');
    // 中间 CNOT 的控制位为 __ctrl、目标为 q
    assert.strictEqual(steer.body[1].expression.arguments[0].name, '__ctrl');
    assert.strictEqual(steer.body[1].expression.arguments[1].name, 'q');
    // 外层 H 只作用于目标 q
    assert.strictEqual(steer.body[0].expression.arguments[0].name, 'q');
});

test('gate-synth: y steer decomposes to [crz(-π/2), cnot, crz(π/2)]', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { y(q); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    const names = steer.body.map((s: any) => s.expression.name);
    assert.deepStrictEqual(names, ['crz', 'cnot', 'crz'], 'y steer decomposition');
    const a0 = steer.body[0].expression.arguments[2];
    const a2 = steer.body[2].expression.arguments[2];
    assert.ok(Math.abs(a0.value + Math.PI / 2) < 1e-12, 'first angle = -π/2');
    assert.ok(Math.abs(a2.value - Math.PI / 2) < 1e-12, 'last angle = +π/2');
    assert.strictEqual(steer.body[0].expression.arguments[0].name, '__ctrl');
});

test('gate-synth: s steer is crz(π/2), t steer is crz(π/4)', () => {
    for (const [gate, angle] of [['s', Math.PI / 2], ['t', Math.PI / 4]] as const) {
        const ast = parse(`@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { ${gate}(q); }`);
        synthesizeGates(ast);
        const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
        assert.ok(steer, `expect U_steer for ${gate}`);
        assert.strictEqual(steer.body.length, 1, `${gate} steer is one gate`);
        assert.strictEqual(steer.body[0].expression.name, 'crz');
        const args = steer.body[0].expression.arguments;
        assert.strictEqual(args[0].name, '__ctrl');
        assert.strictEqual(args[1].name, 'q');
        assert.ok(Math.abs(args[2].value - angle) < 1e-12, `${gate} steer angle`);
    }
});

test('gate-synth: rx steer decomposes to [h, crz(θ), h] preserving θ', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { rx(q, 0.3); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    const names = steer.body.map((s: any) => s.expression.name);
    assert.deepStrictEqual(names, ['h', 'crz', 'h'], 'rx steer = H·CRz(θ)·H');
    const theta = steer.body[1].expression.arguments[2];
    assert.strictEqual(theta.type, 'NumberLiteral');
    assert.strictEqual(theta.value, 0.3, 'θ preserved');
});

test('gate-synth: ry steer decomposes to [rz(-π/2), h, crz(θ), h, rz(π/2)]', () => {
    const ast = parse('@[gate] @[steer]\n@layer(time=0, thread=0, coord=(0))\nvoid U(Qubit q) { ry(q, 0.3); }');
    synthesizeGates(ast);
    const steer = ast.body.find(n => (n as any).name === 'U_steer') as any;
    assert.ok(steer, 'expect U_steer');
    const names = steer.body.map((s: any) => s.expression.name);
    assert.deepStrictEqual(names, ['rz', 'h', 'crz', 'h', 'rz'], 'ry steer decomposition');
    const theta = steer.body[2].expression.arguments[2];
    assert.strictEqual(theta.value, 0.3, 'θ preserved');
    const first = steer.body[0].expression.arguments[1];
    const last = steer.body[4].expression.arguments[1];
    assert.ok(Math.abs(first.value + Math.PI / 2) < 1e-12, 'first rz = -π/2');
    assert.ok(Math.abs(last.value - Math.PI / 2) < 1e-12, 'last rz = +π/2');
});

// ─── 量子物理特性（@[coherence]/@[noise]/@[basis]）─────────────────
test('parser: @[coherence] @[noise] @[basis] parsed into physical', () => {
    const ast = parse('@[coherence(100, 50)] @[noise("depolarizing")] @[basis("X")]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.ok(fn.physical, 'expect physical');
    assert.deepStrictEqual(fn.physical.coherence, { t1: 100, t2: 50 });
    assert.strictEqual(fn.physical.noise, 'depolarizing');
    assert.strictEqual(fn.physical.basis, 'X');
    // 物理特性不应残留为系统属性
    assert.strictEqual(fn.attributes.length, 0, 'physical must not be system attributes');
});

test('parser: @[decoherence_free] @[error_correction] parsed', () => {
    const ast = parse('@[decoherence_free] @[error_correction("surface")]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }');
    const fn = ast.body.find(n => n.type === 'FunctionDeclaration') as any;
    assert.strictEqual(fn.physical.decoherenceFree, true);
    assert.strictEqual(fn.physical.errorCorrection, 'surface');
});

test('semantic: @[coherence] rejects T2 > T1', () => {
    const a = new SemanticAnalyzer();
    const ast = parse('@[coherence(50, 100)]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }');
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('E-PHY')), 'expect E-PHY for T2 > T1');
});

test('semantic: @[noise] rejects unknown model', () => {
    const a = new SemanticAnalyzer();
    const ast = parse('@[noise("mystery")]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }');
    a.analyze(ast);
    assert.ok(a.errors.some(e => e.message.includes('E-PHY')), 'expect E-PHY for unknown noise');
});

test('ir: @[noise] injects apply_noise after gates', () => {
    const src = '@[noise("depolarizing")]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { h(q); }';
    const ast = parse(src);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('declare void @__quantum__qis__apply_noise'), 'expect declare');
    assert.ok(ir.includes('call void @__quantum__qis__apply_noise'), 'expect apply_noise call after h');
    assert.ok(ir.includes('i32 0'), 'expect channel 0 (depolarizing)');
});

test('ir: @[coherence] maps to amplitude_damping channel', () => {
    const src = '@[coherence(100, 50)]\n@layer(time=0, thread=0, coord=(0))\nvoid f(Qubit q) { x(q); }';
    const ast = parse(src);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('call void @__quantum__qis__apply_noise'), 'expect apply_noise call');
    assert.ok(ir.includes('i32 2'), 'expect channel 2 (amplitude_damping)');
});

// ─── 端到端：可逆编织 demo（@[gate]+@[undo]+@[steer]+@[noise]）────────
test('e2e: reversible weaving demo compiles (undo/steer/noise combined)', () => {
    const src = [
        '@[gate] @[undo] @[steer] @[unitary]',
        'void U(Qubit q) { h(q); rz(q, 0.5); }',
        '',
        '@[gate] @[noise("depolarizing")] @[undo]',
        'void NoisyX(Qubit q) { x(q); }',
        '',
        '@layer(time=0, thread=0, coord=(0))',
        'int32 rvdl_main() {',
        '    Qubit q = alloc();',
        '    Qubit c = alloc();',
        '    U(q);',
        '    U_undo(q);',
        '    U_steer(c, q);',
        '    NoisyX(q);',
        '    NoisyX_undo(q);',
        '    int32 r = measure(q);',
        '    measure(c);',
        '    return r;',
        '}',
    ].join('\n');
    const ast = parse(src);
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    assert.deepStrictEqual(a.errors, [], 'expect no semantic errors');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('define void @U_undo'), 'expect U_undo synthesized');
    assert.ok(ir.includes('define void @U_steer'), 'expect U_steer synthesized');
    assert.ok(ir.includes('call void @U_undo'), 'expect call U_undo');
    assert.ok(ir.includes('call void @U_steer'), 'expect call U_steer');
});
