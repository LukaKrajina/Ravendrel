import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { buildTopology } from './topology';
import { IRGenerator } from './ir';
import { buildMir } from './mir';
import { serializeMir } from './mir-serialize';

function parse(src: string) {
    return new Parser(new Lexer(src)).parse();
}

function topo(src: string) {
    return buildTopology(parse(src));
}

/** 一个源块 + 一个（或多个）目标块的标准骨架。
 *  函数体带量子源：srcTag/dstTag 一旦声明 track=，E-TRK002 要求作用域内
 *  存在量子态（无源夹具自 E-TRK002 落地起即为非法程序）。 */
function program(srcTag: string, dstTags: string[] = ['time=1, thread=0, coord=(1,0)']) {
    const dst = dstTags
        .map((t, i) => `@layer(${t})\nint32 g${i}() { auto q = new QuantumRegister(1); return ${i}; }`)
        .join('\n');
    return `@layer(${srcTag})\nint32 f() { auto q = new QuantumRegister(1); return 0; }\n${dst}`;
}

/**
 * 解码 LLVM 字符串常量里的 `\XX` 转义（十六进制字节对）。
 * 直接对未解码的 IR 文本做正则匹配会全部落空 —— IR 把 `"` 写成 `\22`，
 * 于是 `"shuttleLinks":[{` 在原文里根本不存在。这类「测试静默失效」比
 * 断言失败更危险，故此处显式解码。
 */
function decodeLlvmEscapes(s: string): string {
    return s.replace(/\\([0-9A-Fa-f]{2})/g, (_m, h) => String.fromCharCode(parseInt(h, 16)));
}

/** 取出 @rvdl_topology_json 并解析为对象（权威线格式） */
function wire(src: string): any {
    const r = topo(src);
    const ir = (new IRGenerator() as any).emitTopologyIR(r.topology!).join('\n');
    const m = ir.match(/@rvdl_topology_json = .*c"((?:[^"\\]|\\.)*)"/);
    assert.ok(m, '拓扑 JSON 常量应存在');
    const nul = decodeLlvmEscapes(m![1]).indexOf('\u0000');
    return JSON.parse(decodeLlvmEscapes(m![1]).slice(0, nul < 0 ? undefined : nul));
}

// ═══════════════════════════════════════════════════════════════════
// 一、解析：shuttle / track 的语法形态
// ═══════════════════════════════════════════════════════════════════

test('shuttle: shuttle=(1,0) 解析为「一个」二维目标 coord', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0)'));
    assert.strictEqual(w.shuttleLinks.length, 1);
    assert.strictEqual(w.shuttleLinks[0].from, 'f');
    assert.strictEqual(w.shuttleLinks[0].targets.length, 1);
    assert.strictEqual(w.shuttleLinks[0].targets[0].name, 'g0');
    assert.deepStrictEqual(w.shuttleLinks[0].targets[0].coord, [1, 0],
        '(1,0) 必须是**一个** 2 维坐标，而不是两个 1 维坐标 [1] 与 [0]');
});

test('shuttle: shuttle=((1,0),(0,1)) 解析为「两个」目标 coord', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=((1,0),(0,1))',
        ['time=1, thread=0, coord=(1,0)', 'time=2, thread=0, coord=(0,1)']));
    const names = w.shuttleLinks[0].targets.map((t: any) => t.name).sort();
    assert.deepStrictEqual(names, ['g0', 'g1']);
    const coords = w.shuttleLinks[0].targets.map((t: any) => t.coord.join(',')).sort();
    assert.deepStrictEqual(coords, ['0,1', '1,0']);
});

test('shuttle: 显式 shuttle_strategy=-1（文档化默认）可直接书写', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_strategy=-1'));
    assert.strictEqual(w.shuttleLinks[0].strategy, -1);
});

test('track: track=(a, b, c) 解析为可观测量列表', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), track=(expect_z, entropy, num_qubits)'));
    assert.strictEqual(w.trackingScopes.length, 1);
    assert.deepStrictEqual(w.trackingScopes[0].observables, ['expect_z', 'entropy', 'num_qubits']);
});

// ═══════════════════════════════════════════════════════════════════
// 二、关联度核
// ═══════════════════════════════════════════════════════════════════

test('shuttle: w = exp(−d_L1 / lambda)', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_lambda=1'));
    const t = w.shuttleLinks[0].targets[0];
    assert.strictEqual(t.distance, 1);
    assert.ok(Math.abs(t.weight - Math.exp(-1)) < 1e-15);
    assert.ok(Math.abs(t.logWeight - -1) < 1e-15);
});

test('shuttle: lambda=0.5、d=2 → w = exp(−4)', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(2,0), shuttle_lambda=0.5',
        ['time=1, thread=0, coord=(2,0)']));
    const t = w.shuttleLinks[0].targets[0];
    assert.strictEqual(t.distance, 2);
    assert.ok(Math.abs(t.weight - Math.exp(-4)) < 1e-15);
    assert.ok(Math.abs(t.logWeight - -4) < 1e-15);
});

test('shuttle: 高维 coord 的 L1 距离（|Δx|+|Δy|）', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(2,3), shuttle_lambda=1',
        ['time=1, thread=0, coord=(2,3)']));
    const t = w.shuttleLinks[0].targets[0];
    assert.strictEqual(t.distance, 5, 'd_L1 = |2−0| + |3−0| = 5');
    assert.ok(Math.abs(t.logWeight - -5) < 1e-15);
});

test('shuttle: logWeight 与 weight 必须同时下发', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0)'));
    const t = w.shuttleLinks[0].targets[0];
    assert.ok('logWeight' in t, 'logWeight 缺失会让下溢对端无法与「被截断」区分');
    assert.ok('weight' in t);
    assert.ok(Math.abs(t.logWeight - Math.log(t.weight)) < 1e-12);
});

test('shuttle: 「哪怕关联度极小也有效」——极远对端 logWeight 仍有限', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(999,0), shuttle_lambda=1',
        ['time=1, thread=0, coord=(999,0)']));
    const t = w.shuttleLinks[0].targets[0];
    assert.strictEqual(t.logWeight, -999);
    assert.ok(Number.isFinite(t.logWeight));
    assert.strictEqual(t.weight, 0, '线性形式在 d/λ=999 时双精度下溢为 0（数值零，非语义截断）');
});

test('shuttle: 极小关联度不产生任何截断阈值', () => {
    // 连续扫描：只要 d/λ 在可表示范围内，w 必须严格 > 0
    for (const d of [1, 10, 100, 700, 745]) {
        const w = wire(program(`time=0, thread=0, coord=(0), shuttle=(${d}), shuttle_lambda=1`,
            [`time=1, thread=0, coord=(${d})`]));
        const t = w.shuttleLinks[0].targets[0];
        assert.ok(t.weight > 0, `d=${d} 时 w 必须 > 0（无截断阈值）`);
    }
});

// ═══════════════════════════════════════════════════════════════════
// 三、轮次与相位
// ═══════════════════════════════════════════════════════════════════

test('shuttle: auto 轮次 = ceil(log2(span))，span=4 → 2 轮', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,1)',
        ['time=1, thread=0, coord=(1,1)']));
    assert.strictEqual(w.shuttleLinks[0].rounds, 2);
    assert.deepStrictEqual(w.shuttleLinks[0].slotsPerPhase, [2, 2]);
});

test('shuttle: span=2 → 1 轮', () => {
    const w = wire(program('time=0, thread=0, coord=(0), shuttle=(1)',
        ['time=1, thread=0, coord=(1)']));
    assert.strictEqual(w.shuttleLinks[0].rounds, 1);
});

test('shuttle: 负 coord 的跨度按格点真实跨度算（不静默退化）', () => {
    // 地址 −2 与 −1 的跨度是 2 → 1 轮；若按 coord_max+1 会得 span=1 → 0 轮
    const w = wire(program('time=0, thread=0, coord=(-2), shuttle=(-1)',
        ['time=1, thread=0, coord=(-1)']));
    const t = w.shuttleLinks[0].targets[0];
    assert.deepStrictEqual(t.coord, [-1]);
    assert.strictEqual(t.distance, 1);
    assert.strictEqual(w.shuttleLinks[0].rounds, 1, '全负地址不得退化为 0 轮');
});

test('shuttle: shuttle_rounds 显式覆盖 auto', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_rounds=5'));
    assert.strictEqual(w.shuttleLinks[0].rounds, 5);
});

test('shuttle: phase=2（默认）→ slotsPerPhase=[r,r]', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0)'));
    assert.strictEqual(w.shuttleLinks[0].phase, 2);
    assert.deepStrictEqual(w.shuttleLinks[0].slotsPerPhase, [1, 1]);
});

test('shuttle: phase=1 → slotsPerPhase=[0,r]', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_phase=1'));
    assert.strictEqual(w.shuttleLinks[0].phase, 1);
    assert.deepStrictEqual(w.shuttleLinks[0].slotsPerPhase, [0, 1]);
});

test('shuttle: phase=0 → slotsPerPhase=[r,0]', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_phase=0'));
    assert.strictEqual(w.shuttleLinks[0].phase, 0);
    assert.deepStrictEqual(w.shuttleLinks[0].slotsPerPhase, [1, 0]);
});

test('shuttle: 默认 strategy = auto（-1）且五种策略均可书写', () => {
    const dflt = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0)'));
    assert.strictEqual(dflt.shuttleLinks[0].strategy, -1, '默认必须是 auto');
    for (const s of [-1, 0, 1, 2, 3, 4]) {
        const w = wire(program(`time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_strategy=${s}`));
        assert.strictEqual(w.shuttleLinks[0].strategy, s);
    }
});

// ─── 策略解析深度：轮次必须按所选策略算 ────────────────────────────────
// 这一组守的是一处**真实存在过的缺陷**：轮次此前一律套用蝶式公式 ⌈log₂span⌉，
// 而环式的实际轮数是 n、树式是 ⌈log₂(n+1)⌉。轮次既是执行量又是 E-SHU007 的
// 诊断量 —— 统一套一个式子会让 deadline 检查**系统性低估**环式代价。
// 覆盖率为零是这处缺陷能存活的原因，故此处逐策略、逐规模钉死。

/** 造一个源块 + n 个 1 维对端（coord=(1)..(n)）的骨架。
 *  函数体带量子源（E-TRK002 判据，同 program()）。 */
function fanout(tag: string, n: number): string {
    const coords = Array.from({ length: n }, (_v, i) => `(${i + 1})`).join(',');
    const dst = Array.from({ length: n }, (_v, i) =>
        `@layer(time=1, thread=0, coord=(${i + 1}))\nint32 g${i}() { auto q = new QuantumRegister(1); return ${i}; }`).join('\n');
    // 多目标须写成 coord 组：`((1),(2),(3))`。
    // 写成 `(1,2,3)` 会被解析为**一个** 3 维坐标 —— 正是这个区别让
    // 「N 个对端」与「1 个 N 维对端」在语法上分得开。
    return `@layer(${tag})\nint32 f() { auto q = new QuantumRegister(1); return 0; }\n${dst}`;
}

/** 多目标 shuttle 的坐标组写法 */
function groups(n: number): string {
    return '(' + Array.from({ length: n }, (_v, i) => `(${i + 1})`).join(',') + ')';
}

test('depth: 蝶式轮次 = ⌈log₂span⌉（与 n 无关）', () => {
    // span 由格点跨度决定，不由对端数决定
    for (const [span, want] of [[2, 1], [4, 2], [8, 3], [16, 4], [64, 6]] as const) {
        const dim = span - 1;
        const w = wire(program(`time=0, thread=0, coord=(0), shuttle=(${dim}), shuttle_strategy=0`,
            [`time=1, thread=0, coord=(${dim})`]));
        assert.strictEqual(w.shuttleLinks[0].rounds, want, `span=${span} → 蝶式 ${want} 轮`);
    }
});

test('depth: 环式轮次 = 对端数 n（**不是** ⌈log₂span⌉）', () => {
    // n 个对端铺在 coord=1..n → span=n+1 → 蝶式是 ⌈log₂(n+1)⌉ 轮，环式必须是 n 轮
    for (const n of [1, 2, 3, 8, 16]) {
        const w = wire(fanout(`time=0, thread=0, coord=(0), shuttle=${groups(n)}, shuttle_strategy=1`, n));
        assert.strictEqual(w.shuttleLinks[0].targets.length, n);
        assert.strictEqual(w.shuttleLinks[0].rounds, n, `环式 ${n} 个对端 → ${n} 轮（绕环一圈）`);
    }
});

test('depth: 树式轮次 = ⌈log₂(n+1)⌉（2^d−1 ≥ n）', () => {
    for (const [n, want] of [[1, 1], [2, 2], [3, 2], [4, 3], [7, 3], [8, 4], [16, 5]] as const) {
        const w = wire(fanout(`time=0, thread=0, coord=(0), shuttle=${groups(n)}, shuttle_strategy=2`, n));
        assert.strictEqual(w.shuttleLinks[0].rounds, want,
            `树式 ${n} 个对端 → ⌈log₂${n + 1}⌉ = ${want} 轮`);
    }
});

test('depth: Grover 轮次 = 1（O(1)，与规模和 span 无关）', () => {
    for (const n of [1, 4, 16]) {
        const w = wire(fanout(`time=0, thread=0, coord=(0), shuttle=${groups(n)}, shuttle_strategy=3`, n));
        assert.strictEqual(w.shuttleLinks[0].rounds, 1, 'Grover 恒 1 轮');
    }
});

// ─── c001 蝶点（strategy=4）：压缩成点再辐射 ──────────────────────────────
// 蝶式让数据在 ⌈log₂span⌉ 轮里逐位翻转 —— 远端对端要 log₂d 轮才收到；
// c001 把全体关联层的主要特征一次性汇入蝶点再由蝶点辐射，深度恒 1 轮，
// 与 span / 对端数全部无关，且零附加资源（区别于需 2ⁿ 寄存器的 Grover）。
// 这正是 docs §3.7 的「DNA 链多层架构」基元：多个蝶点经拓扑边互联成链。

test('depth: c001 蝶点轮次恒 = 1（与 span / 对端数无关）', () => {
    for (const n of [1, 4, 16, 64]) {
        const w = wire(fanout(`time=0, thread=0, coord=(0), shuttle=${groups(n)}, shuttle_strategy=4`, n));
        assert.strictEqual(w.shuttleLinks[0].targets.length, n);
        assert.strictEqual(w.shuttleLinks[0].rounds, 1, `c001 ${n} 个对端 → 恒 1 轮（压缩成点再辐射）`);
    }
    // 远端对端（span 大）同样是 1 轮 —— 蝶式此时要 ⌈log₂span⌉ 轮
    const far = wire(program('time=0, thread=0, coord=(0), shuttle=(1000), shuttle_strategy=4',
        ['time=1, thread=0, coord=(1000)']));
    assert.strictEqual(far.shuttleLinks[0].rounds, 1, '远端对端（span=1001）仍 1 轮即达');
});

test('depth: c001 比蝶式更浅（span 越大差距越大）—— 且比 Grover 少资源需求', () => {
    // span=65（coord 0..64）→ 蝶式 ⌈log₂65⌉ = 7 轮；c001 恒 1 轮
    const tag = (s: number) => wire(program(
        `time=0, thread=0, coord=(0), shuttle=(64), shuttle_strategy=${s}`,
        ['time=1, thread=0, coord=(64)'])).shuttleLinks[0].rounds;
    assert.strictEqual(tag(0), 7, '蝶式跟 span 走 → 7 轮');
    assert.strictEqual(tag(4), 1, 'c001 蝶点恒 1 轮');
    assert.ok(tag(4) < tag(0), 'c001 在远端拓扑下严格更浅');
});

test('depth: c001 显式 rounds 仍被尊重（用户值即权威值，= 多次完整汇聚+辐射）', () => {
    const w = wire(fanout(
        `time=0, thread=0, coord=(0), shuttle=${groups(4)}, shuttle_strategy=4, shuttle_rounds=3`, 4));
    assert.strictEqual(w.shuttleLinks[0].rounds, 3);
});

test('depth: 策略确实改变轮次（否则 shuttle_strategy 对调度是哑参数）', () => {
    // n=8、span=9：蝶式 4 轮 / 环式 8 轮 / 树式 4 轮 / Grover 1 轮 / c001 1 轮
    const rounds = [-1, 0, 1, 2, 3, 4].map(s => wire(fanout(
        `time=0, thread=0, coord=(0), shuttle=${groups(8)}, shuttle_strategy=${s}`, 8)).shuttleLinks[0].rounds);
    const [auto, butterfly, ring, tree, grover, c001] = rounds;
    assert.strictEqual(auto, butterfly, 'auto 的默认落点与蝶式一致（运行期启发式的默认策略）');
    assert.strictEqual(grover, 1, 'Grover 最短');
    assert.strictEqual(c001, 1, 'c001 蝶点同为 1 轮');
    assert.notStrictEqual(ring, butterfly, '环式必须与蝶式不同 —— 否则该旋钮对调度无影响');
    assert.strictEqual(ring, 8, '环式 = n');
    assert.strictEqual(butterfly, 4, '蝶式 = ⌈log₂9⌉');
    assert.strictEqual(tree, 4, '树式 = ⌈log₂9⌉');
    // c001 与环式/蝶式都不同（此拓扑下蝶式 4 轮）—— 旋钮确实生效
    assert.notStrictEqual(c001, butterfly, 'c001 必须与蝶式不同（此拓扑蝶式 4 轮）');
});

test('depth: 环式/树式也存在「比蝶式更浅」的拓扑（不存在普适深度排序）', () => {
    // 对端少而相距远：1 个对端在 coord=(1000) → span=1001 → 蝶式 ⌈log₂1001⌉ = 10 轮，
    // 环式 = 1 轮。若统一套蝶式公式，E-SHU007 会把环式高估 10 倍而**误报**。
    const cols = ['time=1, thread=0, coord=(1000)'];
    const ring = wire(program('time=0, thread=0, coord=(0), shuttle=(1000), shuttle_strategy=1', cols));
    assert.strictEqual(ring.shuttleLinks[0].rounds, 1, '环式 1 个对端 → 1 轮');

    const bf = wire(program('time=0, thread=0, coord=(0), shuttle=(1000), shuttle_strategy=0', cols));
    assert.strictEqual(bf.shuttleLinks[0].rounds, 10, '蝶式跟 span 走 → 10 轮');
    assert.ok(bf.shuttleLinks[0].rounds > ring.shuttleLinks[0].rounds,
        '同一拓扑下蝶式（跟 span 走）比环式（跟 n 走）更深');
});

test('depth: 退化 span（单格点）仍至少 1 轮 —— 0 轮是静默失效', () => {
    // coord 全为 0、对端靠 time/dual 区分 → span=1。
    // 解析为 0 轮意味着声明的对端一个字节都收不到，且用户看不到任何报错。
    const w = wire(program('time=0, thread=0, coord=(0), shuttle=(0), shuttle_strategy=0',
        ['time=1, thread=0, coord=(0), dual=1']));
    assert.strictEqual(w.shuttleLinks[0].rounds, 1, 'span=1 且声明了对端 → 至少 1 轮');
});

test('depth: shuttle_rounds 覆盖任何策略的自动轮次', () => {
    for (const s of [0, 1, 2, 3]) {
        const w = wire(fanout(
            `time=0, thread=0, coord=(0), shuttle=${groups(4)}, shuttle_strategy=${s}, shuttle_rounds=7`, 4));
        assert.strictEqual(w.shuttleLinks[0].rounds, 7,
            `strategy=${s} 时显式 rounds=7 必须被尊重（用户值即权威值）`);
    }
});

test('depth: E-SHU007 用所选策略的轮次（环式不再被低估）', () => {
    // 环式 8 轮 → phase=2 共 16 槽，deadline=4 放不下 → 必须报
    const ring = topo(fanout(
        `time=0, thread=0, coord=(0), cost=1, deadline=4, shuttle=${groups(8)}, shuttle_strategy=1`, 8));
    assert.ok(ring.errors.some(e => e.code === 'E-SHU007'), '环式 8 轮超 deadline=4 → E-SHU007');

    // 同一拓扑下 Grover 只需 1 轮 → 2 槽，放得下 → 不得报。
    // 若这里报错，说明轮次没有按策略算（仍统一套蝶式的 ⌈log₂span⌉）。
    const grover = topo(fanout(
        `time=0, thread=0, coord=(0), cost=1, deadline=4, shuttle=${groups(8)}, shuttle_strategy=3`, 8));
    assert.ok(!grover.errors.some(e => e.code === 'E-SHU007'),
        'Grover 1 轮只占 2 槽，deadline=4 应放得下');
});

test('shuttle: targets 按块名排序（确定性——线格式可复现）', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=((1,0),(0,1))',
        ['time=2, thread=0, coord=(0,1)', 'time=1, thread=0, coord=(1,0)']));
    const names = w.shuttleLinks[0].targets.map((t: any) => t.name);
    assert.deepStrictEqual(names, [...names].sort(), 'targets 必须按块名升序');
});

// ═══════════════════════════════════════════════════════════════════
// 四、追踪的保真阶梯
// ═══════════════════════════════════════════════════════════════════

test('track: auto 按族选级 —— Pauli→L0、统计→L1、经典→L2', () => {
    const w = wire(program(
        'time=0, thread=0, coord=(0,0), track=(expect_z, entropy, num_qubits)'));
    assert.deepStrictEqual(w.trackingScopes[0].level, [0, 1, 2]);
});

test('track: expect_x / expect_y 同属 Pauli 族 → L0', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), track=(expect_x, expect_y)'));
    assert.deepStrictEqual(w.trackingScopes[0].level, [0, 0]);
});

test('track: measure_z/x/y 归 L1（相位不可恢复，按最弱保证才诚实）', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), track=(measure_z, measure_x, measure_y)'));
    assert.deepStrictEqual(w.trackingScopes[0].level, [1, 1, 1]);
});

test('track: shannon4/shannon8/amplitude 属经典量 → L2', () => {
    const w = wire(program(
        'time=0, thread=0, coord=(0,0), track=(shannon4, shannon8, amplitude)'));
    assert.deepStrictEqual(w.trackingScopes[0].level, [2, 2, 2]);
});

test('track: fidelity / attention 归 L1', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), track=(fidelity, attention)'));
    assert.deepStrictEqual(w.trackingScopes[0].level, [1, 1]);
});

test('track: level 与 observables 一一对应（长度必须相等）', () => {
    const w = wire(program(
        'time=0, thread=0, coord=(0,0), track=(expect_z, measure_x, entropy, num_qubits)'));
    const s = w.trackingScopes[0];
    assert.strictEqual(s.level.length, s.observables.length);
    assert.deepStrictEqual(s.level, [0, 1, 1, 2]);
});

test('track: window = [time, time+cost] 界定功能周期', () => {
    const w = wire(program('time=3, thread=0, coord=(0,0), cost=4, track=(expect_z)'));
    assert.deepStrictEqual(w.trackingScopes[0].window, [3, 7]);
});

test('track: track_slots 原样下发，默认 1', () => {
    const dflt = wire(program('time=0, thread=0, coord=(0,0), track=(expect_z)'));
    assert.strictEqual(dflt.trackingScopes[0].slots, 1);
    const explicit = wire(program('time=0, thread=0, coord=(0,0), track=(expect_z), track_slots=4'));
    assert.strictEqual(explicit.trackingScopes[0].slots, 4);
});

test('track: track_mode 原样下发，默认 0（auto）', () => {
    const dflt = wire(program('time=0, thread=0, coord=(0,0), track=(expect_z)'));
    assert.strictEqual(dflt.trackingScopes[0].mode, 0);
    for (const m of [1, 2, 3]) {
        const w = wire(program(`time=0, thread=0, coord=(0,0), track=(expect_z), track_mode=${m}`));
        assert.strictEqual(w.trackingScopes[0].mode, m);
    }
});

// ═══════════════════════════════════════════════════════════════════
// 五、诊断
// ═══════════════════════════════════════════════════════════════════

const codes = (src: string) => topo(src).errors.map(e => e.code);

test('E-SHU001: 目标 coord 无任何块匹配', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(9,9)'))
        .includes('E-SHU001'));
});

test('E-SHU001: 诊断文本含目标 coord（可定位）', () => {
    const e = topo(program('time=0, thread=0, coord=(0,0), shuttle=(9,9)'))
        .errors.find(x => x.code === 'E-SHU001');
    assert.ok(e && /9/.test(e.message));
});

test('E-SHU003: lambda = 0', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_lambda=0'))
        .includes('E-SHU003'));
});

test('E-SHU003: lambda < 0（负数不再崩在解析层）', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_lambda=-2'))
        .includes('E-SHU003'));
});

test('E-SHU004: phase ∉ {0,1,2}', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_phase=7'))
        .includes('E-SHU004'));
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_phase=-1'))
        .includes('E-SHU004'));
});

test('E-SHU005: strategy ∉ {-1,0,1,2,3,4}', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_strategy=9'))
        .includes('E-SHU005'));
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_strategy=-2'))
        .includes('E-SHU005'));
    // 4（c001 蝶点）合法 —— 不得误报
    assert.ok(!codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_strategy=4'))
        .includes('E-SHU005'));
});

test('E-SHU006: rounds < 0', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_rounds=-3'))
        .includes('E-SHU006'));
});

test('E-SHU007: 规划轮次使执行超出 deadline', () => {
    // 源 time=0、cost=1、deadline=1；auto 轮次 2 → 0+1+2 > 1
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), cost=1, deadline=1, shuttle=(1,1)',
        ['time=1, thread=0, coord=(1,1)'])).includes('E-SHU007'));
});

test('E-SHU007: 不超 deadline 时不报（避免误报）', () => {
    assert.ok(!codes(program('time=0, thread=0, coord=(0,0), cost=1, deadline=100, shuttle=(1,1)',
        ['time=1, thread=0, coord=(1,1)'])).includes('E-SHU007'));
});

test('E-SHU008: 对端与本块同 coord + 同 time', () => {
    const src = `@layer(time=0, thread=0, coord=(0), shuttle=(0))
int32 f() { return 0; }
@layer(time=0, thread=1, coord=(0))
int32 g0() { return 1; }`;
    assert.ok(codes(src).includes('E-SHU008'));
});

test('E-TRK001: track=(...) 为空', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), track=()')).includes('E-TRK001'));
});

test('E-TRK001: track_slots < 1', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), track=(expect_z), track_slots=0'))
        .includes('E-TRK001'));
});

test('E-TRK001: track_slots 为负', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), track=(expect_z), track_slots=-1'))
        .includes('E-TRK001'));
});

test('E-TRK003: 未知可观测量标识符', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), track=(expect_w)'))
        .includes('E-TRK003'));
});

test('E-TRK004: track_mode ∉ {0,1,2,3}', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), track=(expect_z), track_mode=9'))
        .includes('E-TRK004'));
});

test('E-TRK003: qnd 强制模式用于非 Pauli 族可观测量', () => {
    // 非破坏读出对非 Pauli 族不存在 → 必须报错而非给出假承诺
    assert.ok(codes(program('time=0, thread=0, coord=(0,0), track=(entropy), track_mode=1'))
        .includes('E-TRK003'));
});

test('E-TRK003: qnd 强制模式用于 Pauli 族则不报（不误报）', () => {
    assert.deepStrictEqual(
        codes(program('time=0, thread=0, coord=(0,0), track=(expect_z), track_mode=1')), []);
});

// ═══════════════════════════════════════════════════════════════════
// 六、E-TOP008 / E-TOP009
// ═══════════════════════════════════════════════════════════════════

test('E-TOP008: 拼错的键被报错而非静默丢弃', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0), thread2=1')).includes('E-TOP008'));
});

test('E-TOP008: 诊断文本包含键名（可定位）', () => {
    const e = topo(program('time=0, thread=0, coord=(0), coordd=(1)'))
        .errors.find(x => x.code === 'E-TOP008');
    assert.ok(e && e.message.includes('coordd'));
});

test('E-TOP008: 已知键不触发（严格白名单，以免误报）', () => {
    const got = codes(program(
        'time=0, thread=0, coord=(0), cost=1, deadline=5, dual=1, ' +
        'shuttle=(1), shuttle_lambda=1, shuttle_phase=2, shuttle_rounds=0, shuttle_strategy=-1, ' +
        'track=(expect_z), track_mode=0, track_slots=1',
        ['time=1, thread=0, coord=(1)']));
    assert.deepStrictEqual(got, []);
});

test('E-TOP008: 未知键的值会被完整跳过（不破坏后续键解析）', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), bogus=(1,2), shuttle=(1,0)'));
    assert.strictEqual(w.shuttleLinks.length, 1, 'bogus 之后声明的 shuttle 仍须被解析');
    assert.strictEqual(w.blocks[0].time, 0);
});

test('E-TOP009: time < 0 报错（此前是无码的解析崩溃）', () => {
    assert.ok(codes(program('time=-1, thread=0, coord=(0)')).includes('E-TOP009'));
});

test('E-TOP009: cost < 0 报错（负 cost 会让功能周期反向）', () => {
    assert.ok(codes(program('time=0, thread=0, coord=(0), cost=-2')).includes('E-TOP009'));
});

test('E-TOP009: thread < 0 报错', () => {
    assert.ok(codes(program('time=0, thread=-1, coord=(0)')).includes('E-TOP009'));
});

test('E-TOP009: 负 coord 是合法的（格点地址，不是计数）', () => {
    assert.deepStrictEqual(
        codes(program('time=0, thread=0, coord=(-1)', ['time=1, thread=0, coord=(-2)'])), []);
});

// ═══════════════════════════════════════════════════════════════════
// 七、惰性：既有语义必须逐字节不变
// ═══════════════════════════════════════════════════════════════════

test('惰性: 未声明两机制时 shuttleLinks / trackingScopes 为空数组且 errors=[]', () => {
    const src = program('time=0, thread=0, coord=(0)');
    assert.deepStrictEqual(topo(src).errors, []);
    const w = wire(src);
    assert.deepStrictEqual(w.shuttleLinks, []);
    assert.deepStrictEqual(w.trackingScopes, []);
});

test('惰性: 既有 @layer 键组合仍产 errors=[]，且 cost/deadline 原样', () => {
    const src = program('time=2, thread=1, coord=(0,1), cost=3, deadline=9, dual=1');
    assert.deepStrictEqual(topo(src).errors, []);
    const w = wire(src);
    assert.strictEqual(w.blocks[0].cost, 3);
    assert.strictEqual(w.blocks[0].deadline, 9);
    assert.deepStrictEqual(w.blocks[0].coord, [0, 1]);
});

test('惰性: dual=true 不再崩溃（此前是无码 Parser Error）', () => {
    assert.deepStrictEqual(topo(program('time=0, thread=0, coord=(0), dual=true')).errors, []);
});

test('惰性: shape 仍从原点起算（不因负 coord 支持而改变既有含义）', () => {
    const only = (tag: string) => `@layer(${tag})\nint32 f() { return 0; }`;
    assert.deepStrictEqual(wire(only('time=0, thread=0, coord=(1)')).shape.coord, [2],
        'shape.coord = coord_max + 1（既有含义）');
    assert.deepStrictEqual(wire(only('time=0, thread=0, coord=(1,0)')).shape.coord, [2, 1]);
});

// ═══════════════════════════════════════════════════════════════════
// 八、MIR 下沉路径必须同款携带（否则静默丢字段）
// ═══════════════════════════════════════════════════════════════════

test('MIR: shuttle 全部字段原样保留', () => {
    const src = `@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_lambda=0.5,
                       shuttle_phase=1, shuttle_rounds=3, shuttle_strategy=2)
int32 f() { return 0; }
@layer(time=1, thread=0, coord=(1,0))
int32 g0() { return 1; }`;
    const doc = JSON.parse(serializeMir(buildMir(parse(src))));
    const f = doc.bodies.find((b: any) => b.owner === 'f');
    assert.ok(f.layer, 'MIR 必须携带 layer');
    assert.deepStrictEqual(f.layer.shuttle.targets, [[1, 0]]);
    assert.strictEqual(f.layer.shuttle.lambda, 0.5);
    assert.strictEqual(f.layer.shuttle.phase, 1);
    assert.strictEqual(f.layer.shuttle.rounds, 3);
    assert.strictEqual(f.layer.shuttle.strategy, 2);
});

test('MIR: track 全部字段原样保留', () => {
    const src = `@layer(time=0, thread=0, coord=(0,0), track=(expect_z, entropy), track_mode=2, track_slots=3)
int32 f() { return 0; }`;
    const doc = JSON.parse(serializeMir(buildMir(parse(src))));
    const f = doc.bodies.find((b: any) => b.owner === 'f');
    assert.deepStrictEqual(f.layer.track.observables, ['expect_z', 'entropy']);
    assert.strictEqual(f.layer.track.mode, 2);
    assert.strictEqual(f.layer.track.slots, 3);
});

test('MIR: cost / deadline / coord 维度不再被丢弃（既有隐患回归）', () => {
    const src = `@layer(time=0, thread=0, coord=(0,0))
int32 f() { return 0; }
@layer(time=2, thread=1, coord=(1,1), cost=4, deadline=11)
int32 g0() { return 1; }`;
    const doc = JSON.parse(serializeMir(buildMir(parse(src))));
    const g = doc.bodies.find((b: any) => b.owner === 'g0');
    assert.strictEqual(g.layer.cost, 4);
    assert.strictEqual(g.layer.deadline, 11);
    assert.deepStrictEqual(g.layer.coord, [1, 1]);
    assert.strictEqual(g.layer.time, 2);
    assert.strictEqual(g.layer.thread, 1);
});

test('MIR: 未声明两机制时 layer 上不出现 shuttle / track 键（惰性）', () => {
    const doc = JSON.parse(serializeMir(buildMir(parse(program('time=0, thread=0, coord=(0)')))));
    const f = doc.bodies.find((b: any) => b.owner === 'f');
    assert.ok(!('shuttle' in f.layer));
    assert.ok(!('track' in f.layer));
});

// ═══════════════════════════════════════════════════════════════════
// 九、与既有机制的正交性
// ═══════════════════════════════════════════════════════════════════

test('正交: 不影响 callGraph（调用传播延迟）推导', () => {
    const src = `
@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0))
int32 parent() { return child(); }
@layer(time=1, thread=0, coord=(1,0))
int32 child() { return 1; }`;
    const w = wire(src);
    assert.strictEqual(w.callGraph.length, 1);
    assert.strictEqual(w.callGraph[0].caller, 'parent');
    assert.strictEqual(w.callGraph[0].callee, 'child');
    assert.strictEqual(w.shuttleLinks[0].from, 'parent');
});

test('正交: 穿梭与追踪可同时声明于一块，互不干扰', () => {
    const w = wire(program(
        'time=0, thread=0, coord=(0,0), shuttle=(1,0), shuttle_lambda=2, track=(expect_z, entropy), track_slots=2'));
    assert.strictEqual(w.shuttleLinks.length, 1);
    assert.strictEqual(w.trackingScopes.length, 1);
    assert.strictEqual(w.shuttleLinks[0].lambda, 2);
    assert.deepStrictEqual(w.trackingScopes[0].level, [0, 1]);
});

test('正交: 未声明 track 的块不产生 trackingScope（不凭空造影子）', () => {
    const w = wire(program('time=0, thread=0, coord=(0,0), shuttle=(1,0)'));
    assert.deepStrictEqual(w.trackingScopes, [],
        '未声明的块绝不应有影子作用域');
});
