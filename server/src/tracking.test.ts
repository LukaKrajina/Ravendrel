import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { buildTopology, TRACKABLE_OBSERVABLES } from './topology';
import { IRGenerator } from './ir';
import { buildMir } from './mir';
import { serializeMir } from './mir-serialize';
import { BorrowChecker, BorrowError } from './borrow';

const ALL_OBS = Object.keys(TRACKABLE_OBSERVABLES);

function parse(srcText: string) {
    return new Parser(new Lexer(srcText)).parse();
}
function topo(srcText: string) {
    return buildTopology(parse(srcText));
}
function src(tag: string) {
    // 函数体带一个量子源：track 的每个可观测量都要求作用域内存在量子态
    // （E-TRK002 的判据）。无源夹具从 E-TRK002 落地起即为非法程序。
    return `@layer(${tag})\nint32 f() { auto q = new QuantumRegister(1); return 0; }`;
}
/** 对一个函数体跑 QLT 借用检查 */
function borrowCodes(srcText: string): string[] {
    const prog = buildMir(parse(srcText));
    return new BorrowChecker(prog.bodies[0]).check().map(d => d.code);
}

/** 取调度表里的 trackingScopes（权威线格式） */
function scopes(srcText: string) {
    const r = topo(srcText);
    const ir = (new IRGenerator() as any).emitTopologyIR(r.topology!).join('\n');
    const m = ir.match(/@rvdl_topology_json = .*c"((?:[^"\\]|\\.)*)"/)!;
    const txt = decodeLlvm(m[1]);
    const nul = txt.indexOf('\u0000');
    return JSON.parse(txt.slice(0, nul < 0 ? undefined : nul)).trackingScopes as any[];
}
function decodeLlvm(s: string): string {
    return s.replace(/\\([0-9A-Fa-f]{2})/g, (_x, h) => String.fromCharCode(parseInt(h, 16)));
}
function codes(srcText: string): string[] {
    return topo(srcText).errors.map(e => e.code);
}

// ═══════════════════════════════════════════════════════════════════
// 一、保真阶梯的族分类必须穷尽（新观测量不得默默落到错误级别）
// ═══════════════════════════════════════════════════════════════════

test('阶梯: TRACKABLE_OBSERVABLES 中每个标识符都能被分到唯一级别', () => {
    const s = scopes(src(`time=0, thread=0, coord=(0), track=(${ALL_OBS.join(', ')})`));
    assert.strictEqual(s.length, 1);
    assert.strictEqual(s[0].level.length, ALL_OBS.length,
        '每个观测量都必须有级别 —— 漏一个就会让 level 与 observables 错位');
    for (const lv of s[0].level) {
        assert.ok(lv === 0 || lv === 1 || lv === 2, `级别必须 ∈ {0,1,2}，实际 ${lv}`);
    }
    assert.ok(ALL_OBS.length >= 10, `可观测量表不应退化；实际 ${ALL_OBS.length} 项`);
});

test('阶梯: 级别与可观测量一一对应、顺序一致', () => {
    const obs = ['num_qubits', 'expect_z', 'entropy', 'shannon4', 'measure_x'];
    const s = scopes(src(`time=0, thread=0, coord=(0), track=(${obs.join(', ')})`));
    assert.deepStrictEqual(s[0].observables, obs, '顺序必须与声明一致');
    assert.deepStrictEqual(s[0].level, [2, 0, 1, 2, 1]);
});

test('阶梯: L0 只给 Pauli 族（Peierls 定理的适用范围）', () => {
    const s = scopes(src('time=0, thread=0, coord=(0), track=(expect_z, expect_x, expect_y)'));
    assert.deepStrictEqual(s[0].level, [0, 0, 0]);
});

test('阶梯: L2 只给纯经典量（无需抽取电路）', () => {
    const s = scopes(src('time=0, thread=0, coord=(0), track=(num_qubits, shannon4, shannon8, amplitude)'));
    assert.deepStrictEqual(s[0].level, [2, 2, 2, 2]);
});

test('阶梯: measure_z/x/y 归 L1 而非 L0（相位不可恢复，按最弱保证才诚实）', () => {
    // 单次软克隆不可能：测 Z 得 b 后测量后态为 |b⟩，相位信息已不可恢复。
    // 若把 measure_* 归 L0 就是给出一个物理上做不到的精确承诺。
    const s = scopes(src('time=0, thread=0, coord=(0), track=(measure_z, measure_x, measure_y)'));
    assert.deepStrictEqual(s[0].level, [1, 1, 1],
        'measure_* 必须按最弱保证归 L1，不能冒充 L0 精确');
});

test('阶梯: 每个可观测量单独声明时都产生作用域', () => {
    for (const obs of ALL_OBS) {
        assert.strictEqual(scopes(src(`time=0, thread=0, coord=(0), track=(${obs})`)).length, 1,
            `${obs} 应产生作用域`);
    }
});

// ═══════════════════════════════════════════════════════════════════
// 二、类型约束：影子槽绝不为 QObject（保 borrow.ts 的 QLT 不变量）
// ═══════════════════════════════════════════════════════════════════

test('类型: 追踪不得新增或豁免任何 QLT 诊断（纯增量）', () => {
    // 影子槽是经典标量缓冲。若实现为 QObject，就会直接违反 E-Q001/E-Q002。
    // 从外部可观测的性质是：声明 track 前后 QLT 诊断集必须逐字相同。
    const cases = [
        // 泄漏：alloc 后未 measure
        'int32 g() { auto q = alloc(); return 0; }',
        // no-cloning：measure 两次
        'int32 g() { auto q = alloc(); int32 a = measure(q); int32 b = measure(q); return a; }',
        // 正常：measure 一次
        'int32 g() { auto q = alloc(); return measure(q); }',
    ];
    for (const body of cases) {
        const plain = borrowCodes(`@layer(time=0, thread=0, coord=(0))\n${body}`).sort();
        const tracked = borrowCodes(
            `@layer(time=0, thread=0, coord=(0), track=(expect_z, entropy), track_slots=3)\n${body}`).sort();
        assert.deepStrictEqual(tracked, plain,
            `追踪必须是纯增量（body: ${body}）`);
    }
});

test('类型: E-Q002 线性泄漏照常报出（追踪不掩盖它）', () => {
    assert.ok(borrowCodes(`@layer(time=0, thread=0, coord=(0), track=(expect_z))
int32 g() { auto q = alloc(); return 0; }`).includes('E-Q002'));
});

test('类型: E-Q001 no-cloning 照常报出（追踪不掩盖它）', () => {
    assert.ok(borrowCodes(`@layer(time=0, thread=0, coord=(0), track=(expect_z))
int32 g() { auto q = alloc(); int32 a = measure(q); int32 b = measure(q); return a; }`)
        .includes('E-Q001'));
});

test('类型: 可观测量的影子槽名不得是 QObject 类型名', () => {
    // 影子槽只承载经典标量。若有人把可观测量表接上 QObject 类型标识符，
    // 这个断言会失败 —— 那是影子槽退化为「量子态副本」的第一个信号。
    //
    // 注意用**精确名**而非子串匹配：`num_qubits` 含 "qubit" 却是一个
    // 完全合法的经典计数（L2/relabel），子串匹配会把它误判为违规。
    const QOBJECT_TYPE_NAMES = new Set(['qobject', 'qstate', 'qreg', 'qubit', 'qbit']);
    for (const obs of ALL_OBS) {
        assert.ok(!QOBJECT_TYPE_NAMES.has(obs.toLowerCase()),
            `可观测量 '${obs}' 是 QObject 类型名，不得作为影子槽载体`);
    }
    assert.ok(ALL_OBS.includes('num_qubits'),
        'num_qubits 是经典计数（L2）—— 必须保留，不能被上面的检查误删');
});

// ═══════════════════════════════════════════════════════════════════
// 三、作用域与功能周期
// ═══════════════════════════════════════════════════════════════════

test('作用域: window = [time, time+cost]，cost 默认 1', () => {
    assert.deepStrictEqual(scopes(src('time=0, thread=0, coord=(0), track=(expect_z)'))[0].window, [0, 1]);
    assert.deepStrictEqual(scopes(src('time=5, thread=0, coord=(0), track=(expect_z)'))[0].window, [5, 6]);
    assert.deepStrictEqual(scopes(src('time=5, thread=0, coord=(0), cost=3, track=(expect_z)'))[0].window, [5, 8]);
});

test('作用域: 每个声明 track 的块各得一个作用域，互不合并', () => {
    const r = topo(`
@layer(time=0, thread=0, coord=(0), track=(expect_z))
int32 a() { auto q = new QuantumRegister(1); return 0; }
@layer(time=1, thread=0, coord=(0), track=(entropy))
int32 b() { auto q = new QuantumRegister(2); return 1; }`);
    const ir = (new IRGenerator() as any).emitTopologyIR(r.topology!).join('\n');
    const dec = (s: string) => s.replace(/\\([0-9A-Fa-f]{2})/g, (_x, h) => String.fromCharCode(parseInt(h, 16)));
    const m = ir.match(/@rvdl_topology_json = .*c"((?:[^"\\]|\\.)*)"/)!;
    const txt = dec(m[1]);
    const ts = JSON.parse(txt.slice(0, txt.indexOf('\u0000'))).trackingScopes;
    assert.strictEqual(ts.length, 2);
    assert.deepStrictEqual(ts.map((s: any) => s.block), ['a', 'b']);
    assert.deepStrictEqual(ts[0].level, [0]);
    assert.deepStrictEqual(ts[1].level, [1]);
});

test('作用域: 同一块上 track 多次声明的观测量被合并', () => {
    const s = scopes(src('time=0, thread=0, coord=(0), track=(expect_z), track=(entropy)'));
    assert.strictEqual(s.length, 1, '同一块只应有一个作用域');
    assert.deepStrictEqual(s[0].observables, ['expect_z', 'entropy']);
    assert.deepStrictEqual(s[0].level, [0, 1]);
});

// ═══════════════════════════════════════════════════════════════════
// 四、强制模式与 auto 的关系
// ═══════════════════════════════════════════════════════════════════

test('模式: track_mode=qnd 对 Pauli 族合法（不误报）', () => {
    assert.deepStrictEqual(topo(src('time=0, thread=0, coord=(0), track=(expect_z), track_mode=1')).errors, []);
    assert.deepStrictEqual(topo(src('time=0, thread=0, coord=(0), track=(expect_x, expect_y), track_mode=1')).errors, []);
});

test('模式: track_mode=qnd 对非 Pauli 族报 E-TRK003（不存在非破坏读出）', () => {
    for (const obs of ['entropy', 'measure_z', 'num_qubits', 'amplitude', 'fidelity', 'attention', 'shannon4']) {
        const codes = topo(src(`time=0, thread=0, coord=(0), track=(${obs}), track_mode=1`)).errors.map(e => e.code);
        assert.ok(codes.includes('E-TRK003'),
            `qnd 强制模式下 ${obs} 无法非破坏读出，必须报 E-TRK003；实际 ${JSON.stringify(codes)}`);
    }
});

test('模式: track_mode=shadow / relabel 对任意已知观测量都不报错', () => {
    for (const m of [2, 3]) {
        for (const obs of ALL_OBS) {
            const codes = topo(src(`time=0, thread=0, coord=(0), track=(${obs}), track_mode=${m}`)).errors.map(e => e.code);
            assert.deepStrictEqual(codes, [], `mode=${m} obs=${obs} 不应报错`);
        }
    }
});

test('模式: auto（默认 0）等价于不写 track_mode', () => {
    const a = scopes(src('time=0, thread=0, coord=(0), track=(expect_z, entropy, num_qubits)'));
    const b = scopes(src('time=0, thread=0, coord=(0), track=(expect_z, entropy, num_qubits), track_mode=0'));
    assert.deepStrictEqual(a, b);
});

test('模式: 强制模式不改变下发级别（级别由实际选中的模式决定）', () => {
    const s = scopes(src('time=0, thread=0, coord=(0), track=(entropy), track_mode=3'));
    assert.deepStrictEqual(s[0].mode, 3);
    assert.deepStrictEqual(s[0].level, [2], 'relabel 强制 → 级别 L2');
});

// ═══════════════════════════════════════════════════════════════════
// 五、与穿梭的耦合：W-SHU001（退化路径必须诚实告警）
// ═══════════════════════════════════════════════════════════════════

test('耦合: 声明 shuttle 但无 track → W-SHU001（量子路径不可用，退化为经典混合）', () => {
    const r = topo(`
@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0))
int32 f() { auto q = new QuantumRegister(1); return 0; }
@layer(time=1, thread=0, coord=(1,0), track=(expect_z))
int32 g0() { auto q = new QuantumRegister(1); return 1; }`);
    assert.ok(r.warnings.map(w => w.code).includes('W-SHU001'),
        `无 @track 的穿梭必须诚实告警而非静默成功；实际 warnings=${JSON.stringify(r.warnings.map(w => w.code))}`);
    // 告警**不得**进入 errors 通道：那会让程序被 `errors.length > 0` 毙掉，
    // 文档承诺的「退化为经典影子混合」永远走不到。
    assert.ok(!r.errors.map(e => e.code).includes('W-SHU001'),
        'W-SHU001 必须走 warning 通道（降级告警不是编译错误）');
});

test('耦合: 同时声明 track 与 shuttle → 不报 W-SHU001', () => {
    const r = topo(`
@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0), track=(expect_z))
int32 f() { auto q = new QuantumRegister(1); return 0; }
@layer(time=1, thread=0, coord=(1,0), track=(expect_z))
int32 g0() { auto q = new QuantumRegister(1); return 1; }`);
    assert.deepStrictEqual(r.errors, []);
    assert.deepStrictEqual(r.warnings, []);
});

test('耦合: 穿梭端与追踪端相互独立（无需两端都声明）', () => {
    // 源有 track、目标无 track：穿梭仍成立（目标只是没有影子落地）
    const r = topo(`
@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0), track=(expect_z))
int32 f() { auto q = new QuantumRegister(1); return 0; }
@layer(time=1, thread=0, coord=(1,0))
int32 g0() { return 1; }`);
    assert.deepStrictEqual(r.errors, []);
    assert.deepStrictEqual(r.warnings, []);
});

// ═══════════════════════════════════════════════════════════════════════
// 五之二、E-TRK002：追踪源在块作用域内不可解析
// ═══════════════════════════════════════════════════════════════════════

test('E-TRK002: 无任何量子源却声明 track → 报错（影子槽永远填不满）', () => {
    // 经典函数体 + track=：可观测量无从求值（如对 int32 调 expect_z）
    const codes = topo(`@layer(time=0, thread=0, coord=(0), track=(expect_z))
int32 f() { return 0; }`).errors.map(e => e.code);
    assert.ok(codes.includes('E-TRK002'),
        `无量子源的 track 必须报 E-TRK002；实际 ${JSON.stringify(codes)}`);
});

test('E-TRK002: 有量子源（构造 / alloc / 内建 / 用户函数）→ 不报', () => {
    const sources = [
        // ① 量子构造
        '@layer(time=0, thread=0, coord=(0), track=(expect_z))\nint32 f() { auto q = new QuantumRegister(2); return 0; }',
        '@layer(time=0, thread=0, coord=(0), track=(entropy))\nint32 f() { auto b = new BellState(); return 0; }',
        // ② alloc
        '@layer(time=0, thread=0, coord=(0), track=(expect_z))\nint32 f() { Qubit q = alloc(); return 0; }',
        // ③ 返回量子态的内建
        '@layer(time=0, thread=0, coord=(0), track=(expect_z))\nint32 f() { auto q = basis_state(0.0, 0.0, 0); return 0; }',
        // ④ 用户函数返回 QObject（调用方提供 producers）
        'QObject make_state() { auto q = new QuantumRegister(1); return q; }\n'
        + '@layer(time=0, thread=0, coord=(0), track=(expect_z))\nint32 f() { auto q = make_state(); return 0; }',
    ];
    for (const s of sources) {
        const codes = topo(s).errors.map(e => e.code);
        assert.ok(!codes.includes('E-TRK002'),
            `有量子源不得报 E-TRK002（宁可漏报不可误报）；源码=${s.slice(0, 60)}… 实际=${JSON.stringify(codes)}`);
    }
});

test('E-TRK002: 量子类型参数也是合法源', () => {
    const codes = topo(`@layer(time=0, thread=0, coord=(0), track=(expect_z))
int32 f(QObject q) { return 0; }`).errors.map(e => e.code);
    // 注：带参数的 @layer 函数不参与拓扑独立调度（ir.ts 口径），此处只验证
    // hasQuantumSource 的参数判据本身不误报。
    assert.ok(!codes.includes('E-TRK002'), 'QObject 参数是合法量子源');
});

// ═══════════════════════════════════════════════════════════════════
// 六、MIR 下沉路径
// ═══════════════════════════════════════════════════════════════════

test('MIR: track 的 observables / mode / slots 全部保留', () => {
    const srcText = `@layer(time=0, thread=0, coord=(0,0), track=(expect_z, entropy, measure_x), track_mode=0, track_slots=3)
int32 f() { return 0; }`;
    const doc = JSON.parse(serializeMir(buildMir(parse(srcText))));
    const f = doc.bodies.find((b: any) => b.owner === 'f');
    assert.deepStrictEqual(f.layer.track.observables, ['expect_z', 'entropy', 'measure_x']);
    assert.strictEqual(f.layer.track.mode, 0);
    assert.strictEqual(f.layer.track.slots, 3);
});

test('MIR: 追踪的 window 由 time/cost 在运行期重算（不含硬编码窗口）', () => {
    const doc = JSON.parse(serializeMir(buildMir(parse(
        `@layer(time=4, thread=1, coord=(0,0), cost=3, track=(expect_z))\nint32 f() { return 0; }`))));
    const f = doc.bodies.find((b: any) => b.owner === 'f');
    // MIR 只带原始 time/cost；window 由 TopologyBuilder/MIR 镜像在各自一侧推导，
    // 因此两边必须都从同一对输入算出 [4,7) —— 这里守住输入未被改写
    assert.strictEqual(f.layer.time, 4);
    assert.strictEqual(f.layer.cost, 3);
    assert.ok(!('window' in f.layer.track), 'MIR 不应硬编码 window（否则与 cost 改动脱钩）');
});
