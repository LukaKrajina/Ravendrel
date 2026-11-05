import { test } from 'node:test';
import assert from 'node:assert';
import { parseQasm2 } from './migrate/qasm2';
import { emitCircuit } from './migrate/emitter';
import { migrate, detectLang } from './migrate';
import { lowerGate, lowerToBasic } from './migrate/gates';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';

/** 用 rvdl 现有编译管线校验生成的 .rvdl 源码无语义错误 */
function analyze(src: string): SemanticAnalyzer {
    const parser = new Parser(new Lexer(src));
    const ast = parser.parse();
    const analyzer = new SemanticAnalyzer();
    analyzer.analyze(ast);
    return analyzer;
}

const BELL_QASM = `OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
creg c[2];
h q[0];
cx q[0], q[1];
measure q[0] -> c[0];
measure q[1] -> c[1];
`;

test('migrate: parseQasm2 builds correct CircuitIR', () => {
    const ir = parseQasm2(BELL_QASM);
    assert.strictEqual(ir.qubitCount, 2);
    assert.strictEqual(ir.bitCount, 2);
    assert.strictEqual(ir.ops.length, 4);
    assert.deepStrictEqual(ir.ops[0], { kind: 'gate', name: 'h', qubits: [0], params: [] });
    assert.deepStrictEqual(ir.ops[1], { kind: 'gate', name: 'cx', qubits: [0, 1], params: [] });
    assert.deepStrictEqual(ir.ops[2], { kind: 'measure', qubit: 0, bit: 0 });
    assert.deepStrictEqual(ir.ops[3], { kind: 'measure', qubit: 1, bit: 1 });
});

test('migrate: OpenQASM 2.0 Bell state -> valid .rvdl', () => {
    const rvdl = migrate(BELL_QASM, 'openqasm2');
    assert.ok(rvdl.includes('Qubit q0 = alloc();'));
    assert.ok(rvdl.includes('h(q0);'));
    assert.ok(rvdl.includes('cx(q0, q1);'));
    assert.ok(rvdl.includes('c0 = measure(q0);'));
    assert.ok(rvdl.includes('c1 = measure(q1);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, 'generated .rvdl should be semantically valid');
});

test('migrate: parametric gates u3/cz decompose to native rvdl gates', () => {
    // u3(θ,φ,λ) → rz(λ)·ry(θ)·rz(φ)
    const u3 = lowerGate('u3', [0], [0.5, 0.2, 0.1]);
    assert.deepStrictEqual(u3.map(g => g.name), ['rz', 'ry', 'rz']);
    // cz → h(t)·cx·h(t)
    const cz = lowerGate('cz', [0, 1], []);
    assert.deepStrictEqual(cz.map(g => g.name), ['h', 'cx', 'h']);
    // sdg → rz(-π/2)
    const sdg = lowerGate('sdg', [0], []);
    assert.strictEqual(sdg[0].name, 'rz');
    assert.ok((sdg[0].params[0] as number) < 0);
});

test('migrate: custom gate definition emits @[gate] function', () => {
    const qasm = `OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
creg c[2];
gate my_rot(angle) a {
    rz(angle) a;
}
my_rot(0.1) q[0];
my_rot(0.2) q[1];
measure q[0] -> c[0];
measure q[1] -> c[1];
`;
    const rvdl = migrate(qasm, 'openqasm2');
    assert.ok(rvdl.includes('@[gate]'));
    assert.ok(rvdl.includes('void my_rot(Qubit a, double angle)'));
    assert.ok(rvdl.includes('rz(a, angle);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0);
});

test('migrate: new native gates y/z/s/t/rx/ry are semantically valid', () => {
    const rvdl = `@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    Qubit q = alloc();
    h(q);
    x(q);
    y(q);
    z(q);
    s(q);
    t(q);
    rx(q, 0.5);
    ry(q, 0.5);
    rz(q, 0.5);
    int32 m = measure(q);
    return m;
}`;
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: detectLang auto-detects OpenQASM versions', () => {
    assert.strictEqual(detectLang(BELL_QASM), 'openqasm2');
    assert.strictEqual(detectLang('OPENQASM 3.0;\nqubit[2] q;'), 'openqasm3');
    assert.strictEqual(detectLang('DECLARE ro BIT[1]'), 'quil');
    assert.strictEqual(detectLang('namespace Foo { operation Bar() : Unit {} }'), 'qsharp');
    assert.strictEqual(detectLang('int32 x = 1;'), null);
});

test('migrate: emitter round-trips a full circuit with loop', () => {
    const ir = {
        name: 'test',
        qubitCount: 3,
        bitCount: 3,
        classicalVars: [],
        gateDefs: [],
        ops: [
            { kind: 'gate', name: 'h', qubits: [0], params: [] },
            { kind: 'for', varName: 'i', lo: 0, hi: 3, body: [
                { kind: 'gate', name: 'rz', qubits: [0], params: [0.1] },
            ] },
            { kind: 'measure', qubit: 0, bit: 0 },
            { kind: 'measure', qubit: 1, bit: 1 },
            { kind: 'measure', qubit: 2, bit: 2 },
        ] as any,
    };
    const rvdl = emitCircuit(ir);
    assert.ok(rvdl.includes('for (int32 i = 0; i < 3; i = i + 1) {'));
    assert.ok(rvdl.includes('rz(q0, 0.1);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: OpenQASM 3.0 with qubit[] and for loop -> valid .rvdl', () => {
    const qasm3 = `OPENQASM 3.0;
qubit[3] q;
bit[3] c;
h q[0];
for i in [0:3] {
    rx(0.1) q[i];
}
c = measure q;
`;
    const rvdl = migrate(qasm3, 'openqasm3');
    assert.ok(rvdl.includes('Qubit q0 = alloc();'));
    assert.ok(rvdl.includes('rx(q0, 0.1);'));
    assert.ok(rvdl.includes('rx(q1, 0.1);'));
    assert.ok(rvdl.includes('rx(q2, 0.1);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: Quil circuit -> valid .rvdl', () => {
    const quil = `DECLARE ro BIT[2]
H 0
CNOT 0 1
MEASURE 0 ro[0]
MEASURE 1 ro[1]
`;
    const rvdl = migrate(quil, 'quil');
    assert.ok(rvdl.includes('h(q0);'));
    assert.ok(rvdl.includes('cx(q0, q1);'));
    assert.ok(rvdl.includes('c0 = measure(q0);'));
    assert.ok(rvdl.includes('c1 = measure(q1);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: Q# operation -> valid .rvdl', () => {
    const qsharp = `namespace Quantum.Bell {
    operation Bell() : Result[] {
        use (q0, q1) = (Qubit(), Qubit());
        H(q0);
        CNOT(q0, q1);
        let r0 = M(q0);
        let r1 = M(q1);
        Reset(q0);
        Reset(q1);
        return [r0, r1];
    }
}
`;
    const rvdl = migrate(qsharp, 'qsharp');
    assert.ok(rvdl.includes('h(q0);'));
    assert.ok(rvdl.includes('cx(q0, q1);'));
    assert.ok(rvdl.includes('c0 = measure(q0);'));
    assert.ok(rvdl.includes('c1 = measure(q1);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: Silq circuit -> valid .rvdl', () => {
    const silq = `def main() {
    q := 0:𝔹;
    q := H(q);
    x := measure(q);
    return x;
}
`;
    const rvdl = migrate(silq, 'silq');
    assert.ok(rvdl.includes('h(q0);'));
    assert.ok(rvdl.includes('c0 = measure(q0);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: mid-circuit reset uses QObject layer with conditional flip', () => {
    const qasm = `OPENQASM 2.0;
include "qelib1.inc";
qreg q[1];
creg c[2];
h q[0];
measure q[0] -> c[0];
reset q[0];
h q[0];
measure q[0] -> c[1];
`;
    const rvdl = migrate(qasm, 'openqasm2');
    // 中间 reset → QObject 层：QuantumRegister + qgate_* + qmeasure + 条件翻转
    assert.ok(rvdl.includes('auto q = new QuantumRegister(1);'));
    assert.ok(rvdl.includes('c0 = qmeasure(q, 0);'));
    assert.ok(rvdl.includes('qmeasure(q, 0);'));
    assert.ok(rvdl.includes('qgate_x(q, 0);'));
    assert.ok(rvdl.includes('c1 = qmeasure(q, 0);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: OpenQASM 3 classical vars + while + complex if -> valid .rvdl', () => {
    const qasm3 = `OPENQASM 3.0;
qubit[1] q;
bit[1] c;
int[32] count = 0;
bool flag = true;
h q[0];
count = count + 1;
while (count < 3) {
    count = count + 1;
}
if (count >= 1 && flag) {
    x q[0];
}
c[0] = measure q[0];
`;
    const rvdl = migrate(qasm3, 'openqasm3');
    assert.ok(rvdl.includes('int32 count = 0;'));
    assert.ok(rvdl.includes('int32 flag = 1;'));
    assert.ok(rvdl.includes('count = count + 1;'));
    assert.ok(rvdl.includes('while (count < 3) {'));
    assert.ok(rvdl.includes('if (count >= 1 && flag) {'));
    assert.ok(rvdl.includes('c0 = measure(q0);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: gate decomposition to QObject primitives (h/x/rz/cnot)', () => {
    // ry(θ) → rz(-π/2)·h·rz(θ)·h·rz(π/2)
    const ry = lowerToBasic('ry', [0], [0.5]);
    assert.deepStrictEqual(ry.map(g => g.name), ['rz', 'h', 'rz', 'h', 'rz']);
    // swap → 3×cnot
    const sw = lowerToBasic('swap', [0, 1], []);
    assert.deepStrictEqual(sw.map(g => g.name), ['cnot', 'cnot', 'cnot']);
    // y → x·h·x·h
    const y = lowerToBasic('y', [0], []);
    assert.deepStrictEqual(y.map(g => g.name), ['x', 'h', 'x', 'h']);
});

// ═══════════════════════════════════════════════════════════════════
// 迁移器 MVP 缺口补齐后的回归钉住
// ═══════════════════════════════════════════════════════════════════

test('migrate: OpenQASM 3 for-loop step is honored (three-part range)', () => {
    // [0:2:10] 的**中间**数是步长（OpenQASM 3 规范）：i = 0,2,4,6,8。
    // 曾经的实现读成 lo:hi 并丢弃步长 → 静默迁出 i = 0,1 的错误电路。
    const qasm3 = `OPENQASM 3.0;
qubit[1] q;
bit[1] c;
for i in [0:2:6] {
    x q[0];
}
c[0] = measure q[0];
`;
    const rvdl = migrate(qasm3, 'openqasm3');
    // 展开 3 次（i = 0, 2, 4），不是 6 次也不是 2 次
    const xCount = (rvdl.match(/x\(q0\);/g) || []).length;
    assert.strictEqual(xCount, 3, `step=2 over [0,6) should unroll 3 times, got ${xCount}`);
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: OpenQASM 3 for-loop negative step (descending)', () => {
    const qasm3 = `OPENQASM 3.0;
qubit[1] q;
bit[1] c;
for i in [3:-1:0] {
    x q[0];
}
c[0] = measure q[0];
`;
    const rvdl = migrate(qasm3, 'openqasm3');
    const xCount = (rvdl.match(/x\(q0\);/g) || []).length;
    assert.strictEqual(xCount, 3, `descending [3,-1,0) should unroll 3 times (3,2,1), got ${xCount}`);
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: Q# multi-operation migrates the entry, inlines the helper', () => {
    // 曾经的实现只迁**第一个** operation：helper 在前时会静默丢掉 main。
    const qsharp = `namespace Quantum.Teleport {
    operation PreparePair(q0 : Qubit, q1 : Qubit) : Unit {
        H(q0);
        CNOT(q0, q1);
    }
    operation Main() : Unit {
        use (a, b) = (Qubit(), Qubit());
        PreparePair(a, b);
        H(a);
        let r = M(a);
        let r2 = M(b);
        Reset(a);
        Reset(b);
    }
}
`;
    const rvdl = migrate(qsharp, 'qsharp');
    // helper 的 H/CNOT 内联进 main；main 自己的 H(a) 也是 h(q0) —— 共两次
    assert.ok(rvdl.includes('h(q0);'), 'helper H inlined');
    assert.ok(rvdl.includes('cx(q0, q1);'), 'helper CNOT inlined');
    const hCount = (rvdl.match(/h\(q0\);/g) || []).length;
    assert.strictEqual(hCount, 2, `helper H + entry H both present, got ${hCount}`);
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: Q# if/else on measurement result -> valid .rvdl', () => {
    // 曾经的实现把 if 整段跳过（静默丢失经典控制流）。
    const qsharp = `namespace Quantum.Cond {
    operation Main() : Unit {
        use (q0, q1) = (Qubit(), Qubit());
        H(q0);
        let r = M(q0);
        if (r == One) {
            X(q1);
        } else {
            H(q1);
        }
        let r2 = M(q1);
        Reset(q0);
        Reset(q1);
    }
}
`;
    const rvdl = migrate(qsharp, 'qsharp');
    assert.ok(rvdl.includes('if (c0 == 1) {'), `condition should reference the measured bit, got:\n${rvdl}`);
    assert.ok(rvdl.includes('x(q1);'), 'then-branch present');
    assert.ok(rvdl.includes('h(q1);'), 'else-branch present');
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));
});

test('migrate: Q# recursive operation is rejected loudly (not silently dropped)', () => {
    const qsharp = `namespace Quantum.Rec {
    operation A(q : Qubit) : Unit {
        A(q);
    }
    operation Main() : Unit {
        use (q0) = (Qubit());
        A(q0);
    }
}
`;
    assert.throws(() => migrate(qsharp, 'qsharp'), /recursive operation/);
});

test('migrate: Quil DEFGATE matrix body is skipped, use is rejected by name', () => {
    // 矩阵体在缩进续行上；曾经只跳到行尾，矩阵行被当作门调用解析。
    const quil = `DECLARE ro BIT[2];
DEFGATE RX(%theta) AS MATRIX:
    cos(%theta/2), -i*sin(%theta/2)
    -i*sin(%theta/2), cos(%theta/2)
H 0
CNOT 0 1
MEASURE 0 ro[0]
MEASURE 1 ro[1]
`;
    const rvdl = migrate(quil, 'quil');
    assert.ok(rvdl.includes('h(q0);'), 'gates after DEFGATE still migrate');
    assert.ok(rvdl.includes('cx(q0, q1);'));
    const a = analyze(rvdl);
    assert.strictEqual(a.errors.length, 0, a.errors.map(e => e.message).join('; '));

    // 调用 DEFGATE 定义的门 → 指名道姓的拒绝（矩阵门无 rvdl 对应物）
    const quilUsed = `DECLARE ro BIT[1];
DEFGATE MYGATE AS MATRIX:
    0, 1
    1, 0
MYGATE 0
`;
    assert.throws(() => migrate(quilUsed, 'quil'), /DEFGATE \(matrix form\)/);
});
