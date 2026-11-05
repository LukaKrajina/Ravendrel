import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { IRGenerator } from './ir';

function generateIR(src: string): string {
    const parser = new Parser(new Lexer(src));
    const ast = parser.parse();
    return new IRGenerator().generate(ast);
}

test('ir: function with return generates define + ret', () => {
    const ir = generateIR('int32 rvdl_main() { return 42; }');
    assert.ok(ir.includes('define i32 @rvdl_main'));
    assert.ok(ir.includes('ret i32 42'));
});

test('ir: arithmetic generates add instruction', () => {
    const ir = generateIR('int32 rvdl_main() { int32 x = 1 + 2; return x; }');
    assert.ok(ir.includes('add i32 1, 2'));
});

test('ir: comparison generates icmp', () => {
    const ir = generateIR('int32 rvdl_main() { int32 x = 1 < 2; return 0; }');
    assert.ok(ir.includes('icmp slt'));
});

test('ir: qubit allocation generates qubit_allocate call', () => {
    const ir = generateIR('int32 rvdl_main() { auto q = alloc(); h(q); return 0; }');
    assert.ok(ir.includes('__quantum__rt__qubit_allocate'));
    assert.ok(ir.includes('__quantum__qis__h'));
});

test('ir: BellState generates rvdl_create_BellState call', () => {
    const ir = generateIR('int32 rvdl_main() { auto b = new BellState(); return 0; }');
    assert.ok(ir.includes('rvdl_create_BellState'));
});

test('ir: string literal generates global string constant', () => {
    const ir = generateIR('int32 rvdl_main() { let s = "hello"; return 0; }');
    assert.ok(ir.includes('@.str.1'));
    assert.ok(ir.includes('hello'));
});

test('ir: top-level statements wrap into rvdl_main', () => {
    const ir = generateIR('auto q = alloc();');
    assert.ok(ir.includes('define i32 @rvdl_main'));
});

test('ir: QIR standard library declarations present', () => {
    const ir = generateIR('int32 rvdl_main() { return 0; }');
    assert.ok(ir.includes('%Qubit = type opaque'));
    assert.ok(ir.includes('declare i32 @__quantum__qis__measure_int'));
});

test('ir: while loop generates branch labels', () => {
    const ir = generateIR('int32 rvdl_main() { int32 i = 0; while (i < 10) { i = i + 1; } return i; }');
    assert.ok(ir.includes('while_cond_'));
    assert.ok(ir.includes('while_body_'));
    assert.ok(ir.includes('while_after_'));
});

test('ir: measure generates measure_int call', () => {
    const ir = generateIR('int32 rvdl_main() { auto q = alloc(); auto m = measure(q); return m; }');
    assert.ok(ir.includes('__quantum__qis__measure_int'));
});

// ─── QChain 量子区块链内置函数 ──────────────────────────────────────
test('ir: qchain_wallet generates rvdl_qchain_wallet call', () => {
    const ir = generateIR('int32 rvdl_main() { string w = qchain_wallet(); return 0; }');
    assert.ok(ir.includes('declare i8* @rvdl_qchain_wallet'));
    assert.ok(ir.includes('call i8* @rvdl_qchain_wallet'));
});

test('ir: qchain_mint generates rvdl_qchain_mint call with zext amount', () => {
    const ir = generateIR('int32 rvdl_main() { qchain_mint("a", 1000); return 0; }');
    assert.ok(ir.includes('call void @rvdl_qchain_mint'));
    assert.ok(ir.includes('zext i32'));
});

test('ir: qchain_balance declares i64 return', () => {
    const ir = generateIR('int32 rvdl_main() { uint64 b = qchain_balance("a"); return 0; }');
    assert.ok(ir.includes('declare i64 @rvdl_qchain_balance'));
    assert.ok(ir.includes('call i64 @rvdl_qchain_balance'));
});

test('ir: qchain_coin_mint returns QObject pointer', () => {
    const ir = generateIR('int32 rvdl_main() { QObject c = qchain_coin_mint(8); return 0; }');
    assert.ok(ir.includes('declare %QObject* @rvdl_qchain_coin_mint'));
    assert.ok(ir.includes('call %QObject* @rvdl_qchain_coin_mint'));
});

test('ir: qchain_coin_verify takes QObject', () => {
    const ir = generateIR('int32 rvdl_main() { QObject c = qchain_coin_mint(8); int32 ok = qchain_coin_verify(c); return ok; }');
    assert.ok(ir.includes('declare i32 @rvdl_qchain_coin_verify'));
    assert.ok(ir.includes('call i32 @rvdl_qchain_coin_verify'));
});

// ─── QChain 密码原语 / 抗超时空 / 时空加密 ──────────────────────
test('ir: qchain_sha3 generates rvdl_qchain_sha3 call', () => {
    const ir = generateIR('int32 rvdl_main() { string h = qchain_sha3("abc"); return 0; }');
    assert.ok(ir.includes('declare i8* @rvdl_qchain_sha3'));
    assert.ok(ir.includes('call i8* @rvdl_qchain_sha3'));
});

test('ir: qchain_sign generates rvdl_qchain_sign call', () => {
    const ir = generateIR('int32 rvdl_main() { string s = qchain_sign("msg"); return 0; }');
    assert.ok(ir.includes('declare i8* @rvdl_qchain_sign'));
    assert.ok(ir.includes('call i8* @rvdl_qchain_sign'));
});

test('ir: qchain_mlkem_encaps generates rvdl_qchain_mlkem_encaps call', () => {
    const ir = generateIR('int32 rvdl_main() { string c = qchain_mlkem_encaps("pk"); return 0; }');
    assert.ok(ir.includes('declare i8* @rvdl_qchain_mlkem_encaps'));
    assert.ok(ir.includes('call i8* @rvdl_qchain_mlkem_encaps'));
});

test('ir: qchain_causal_verify generates rvdl_qchain_causal_verify call', () => {
    const ir = generateIR('int32 rvdl_main() { int32 ok = qchain_causal_verify(); return ok; }');
    assert.ok(ir.includes('declare i32 @rvdl_qchain_causal_verify'));
    assert.ok(ir.includes('call i32 @rvdl_qchain_causal_verify'));
});

test('ir: qchain_cipher_encrypt zext seed to i64', () => {
    const ir = generateIR('int32 rvdl_main() { string c = qchain_cipher_encrypt(12345, "msg"); return 0; }');
    assert.ok(ir.includes('declare i8* @rvdl_qchain_cipher_encrypt'));
    assert.ok(ir.includes('zext i32'));
    assert.ok(ir.includes('call i8* @rvdl_qchain_cipher_encrypt'));
});

// ─── lattice 晶格数组类型 ──────────────────────────────────────
test('ir: lattice construct generates rvdl_lattice_new call', () => {
    const ir = generateIR('int32 rvdl_main() { lattice<int32, open> b = new lattice<int32, open>(19, 19); return 0; }');
    assert.ok(ir.includes('%Lattice = type opaque'));
    assert.ok(ir.includes('declare %Lattice* @rvdl_lattice_new'));
    assert.ok(ir.includes('call %Lattice* @rvdl_lattice_new(i32 2, i32 19, i32 19, i32 0)'));
});

test('ir: lattice index read/write emits rvdl_lattice_ref/set', () => {
    const ir = generateIR('int32 rvdl_main() { lattice<int32, open> b = new lattice<int32, open>(9, 9); b[3, 3] = 1; int32 v = b[3, 3]; return v; }');
    assert.ok(ir.includes('call void @rvdl_lattice_set'));
    assert.ok(ir.includes('call i32 @rvdl_lattice_ref'));
});

test('ir: lattice periodic boundary encodes 1', () => {
    const ir = generateIR('int32 rvdl_main() { lattice<int32, periodic> w = new lattice<int32, periodic>(9, 9); return 0; }');
    assert.ok(ir.includes('rvdl_lattice_new(i32 2, i32 9, i32 9, i32 1)'));
});

test('ir: lattice introspection emits rank/size/boundary calls', () => {
    const ir = generateIR('int32 rvdl_main() { lattice<int32, open> b = new lattice<int32, open>(19, 19); int32 r = lattice_rank(b); return r; }');
    assert.ok(ir.includes('call i32 @rvdl_lattice_rank'));
});

test('ir: modulo operator emits srem', () => {
    const ir = generateIR('int32 rvdl_main() { int32 m = 10 % 3; return m; }');
    assert.ok(ir.includes('srem i32 10, 3'));
});


// ═══ double → int32 隐式截断（QCOS 量子 UI 依赖此路径）════════════
test('ir: int32 var assigned a double expression emits fptosi', () => {
    // 回归：缺少 fptosi 时会生成 `store i32 %doubleReg` 的非法 IR，
    // 表现为 clang 报 "defined with type 'double' but expected 'i32'"。
    const ir = generateIR([
        'extern double rvdl_math_cos(double x);',
        'int32 scale(double v, int32 r) {',
        '    int32 out = rvdl_math_cos(v) * r;',
        '    return out;',
        '}',
        'int32 entry() { return scale(0.5, 7); }'
    ].join('\n'));
    assert.ok(ir.includes('fptosi double'), 'missing fptosi conversion');
});

test('ir: reassigning a double expression into an int32 local emits fptosi', () => {
    const ir = generateIR([
        'extern double rvdl_math_sqrt(double x);',
        'int32 clamp2(double v) {',
        '    int32 n = 0;',
        '    n = rvdl_math_sqrt(v);',
        '    return n;',
        '}',
        'int32 entry() { return clamp2(2.0); }'
    ].join('\n'));
    assert.ok(ir.includes('fptosi double'), 'missing fptosi on reassignment');
});

test('ir: int32 to double assignment emits sitofp', () => {
    const ir = generateIR([
        'int32 widen(int32 v) {',
        '    double d = v;',
        '    return 0;',
        '}',
        'int32 entry() { return widen(3); }'
    ].join('\n'));
    assert.ok(ir.includes('sitofp i32'), 'missing sitofp conversion');
});


test('ir: tiny float literal keeps a decimal point (1e-9 -> 1.0e-9)', () => {
    // 回归：JS 的 String(1e-9) === '1e-9'，LLVM 要求浮点常量带小数点，
    // 否则报 "integer/byte constant must have integer/byte type"。
    const ir = generateIR([
        'int32 tiny() {',
        '    let x = 0.000000001;',
        '    return 0;',
        '}',
        'int32 entry() { return tiny(); }'
    ].join('\n'));
    assert.ok(!/double 1e-9/.test(ir), 'exponent literal without decimal point');
    assert.ok(ir.includes('0.0e-9') || ir.includes('.0e-9'), 'expected normalized literal');
});

// ============================================================================
// 回归测试：常量折叠与调用点语义
//
// 背景：这四类缺陷都会**静默改变运行时语义**且不报错 —— 编译通过、
// 测试全绿，但数值/边界判断全错。内核侧（QuarkOS qcore）实测踩到过：
//   · fixed 常量引用另一个 fixed → 值变成 0 → 所有边界判断恒真
//   · fixed 常量初始化为常量表达式 → 值变成 0 → 错误码全为 0
//   · double 实参传给 int32 形参 → 生成类型不匹配的 IR，clang 报错
//   · uint32 的 % 与 >> 用了有符号指令 → 无符号运算算出负值
// ============================================================================

test('ir: fixed constant initialised from another fixed keeps its value', () => {
    const ir = generateIR(`
fixed int32 QMEM_BASE = 0x3000;
fixed int32 QMEM_SIZE = 32;
fixed int32 QCAP_MAX = QMEM_SIZE;
int32 rvdl_main() { return QCAP_MAX; }
`);
    assert.ok(ir.includes('@QMEM_SIZE = constant i32 32'), ir);
    assert.ok(ir.includes('@QCAP_MAX = constant i32 32'),
        'fixed-from-fixed must forward the referenced value, not fold to 0:\n' + ir);
    assert.ok(!ir.includes('@QCAP_MAX = constant i32 0'), ir);
});

test('ir: fixed constant initialised from a constant expression is folded', () => {
    const ir = generateIR(`
fixed int32 ERR_NODEV = 0 - 1;
fixed int32 ERR_BUSY  = 0 - 4;
fixed int32 SHIFT    = 2 * 4;
fixed int32 MASK32   = 1 << 8;
int32 rvdl_main() { return ERR_NODEV + ERR_BUSY + SHIFT + MASK32; }
`);
    assert.ok(ir.includes('@ERR_NODEV = constant i32 -1'), ir);
    assert.ok(ir.includes('@ERR_BUSY = constant i32 -4'), ir);
    assert.ok(ir.includes('@SHIFT = constant i32 8'), ir);
    assert.ok(ir.includes('@MASK32 = constant i32 256'), ir);
});

test('ir: negative literal written as subtraction is folded, not zeroed', () => {
    const ir = generateIR(`
fixed int32 CRC_POLY = 0 - 306674912;
int32 rvdl_main() { return CRC_POLY; }
`);
    assert.ok(ir.includes('@CRC_POLY = constant i32 -306674912'), ir);
});

test('ir: double argument to an int32 parameter is coerced with fptosi', () => {
    const ir = generateIR(`
int32 takes_int(int32 v) { return v; }
int32 rvdl_main() { double d = 2.5; return takes_int(d); }
`);
    assert.ok(ir.includes('fptosi double'), ir);
});

test('ir: int32 argument to a double parameter is coerced with sitofp', () => {
    const ir = generateIR(`
double takes_double(double v) { return v; }
int32 rvdl_main() { int32 n = 3; double d = takes_double(n); return 0; }
`);
    assert.ok(ir.includes('sitofp i32'), ir);
});

test('ir: modulo on a uint32 value uses urem, not srem', () => {
    const ir = generateIR(`
extern uint32 rng32();
int32 rvdl_main() { uint32 n = 7; uint32 bucket = n * n; uint32 r = rng32() % bucket; return 0; }
`);
    assert.ok(ir.includes('urem'), ir);
    assert.ok(!ir.includes('srem'), ir);
});

test('ir: right shift on a uint32 value uses lshr, not ashr', () => {
    const ir = generateIR(`
extern uint32 rng32();
int32 rvdl_main() { uint32 v = rng32(); uint32 w = v >> 3; return 0; }
`);
    assert.ok(ir.includes('lshr'), ir);
    assert.ok(!ir.includes('ashr'), ir);
});

test('ir: unsigned comparison uses unsigned predicate', () => {
    const ir = generateIR(`
extern uint32 rng32();
int32 rvdl_main() { uint32 v = rng32(); uint32 lim = 10; if (v < lim) { return 1; } return 0; }
`);
    assert.ok(ir.includes('icmp ult'), ir);
});

test('ir: signed right shift on int32 still uses ashr', () => {
    const ir = generateIR(`
int32 rvdl_main() { int32 v = 0 - 8; int32 w = v >> 2; return w; }
`);
    assert.ok(ir.includes('ashr'), ir);
});

// ═══════════════════════════════════════════════════════════════════
// cgfx_polyline：折线原语（曲线/图表）
//
// 折线必须是**一条路径**而不是循环 cgfx_line：逐段独立描边在每个接缝
// 各画各的帽，夹角处出现缺口或重叠。IR 层的职责是把 arr<i32,N>
// （[N x i32]*）decay 成 i32* 传给 C ABI。
// ═══════════════════════════════════════════════════════════════════

test('ir: cgfx_polyline declares the pointer-first signature', () => {
    const ir = generateIR('int32 rvdl_main() { return 0; }');
    assert.ok(ir.includes('declare void @rvdl_cgfx_polyline(i32*, i32, i32, i32)'), ir);
    assert.ok(ir.includes('declare void @rvdl_cgfx_polyline_a(i32*, i32, i32, i32, i32)'), ir);
});

test('ir: cgfx_polyline decays the array to an element pointer (GEP)', () => {
    const ir = generateIR(`
int32 rvdl_main() {
    arr<int32, 6> pts = [10, 20, 30, 15, 50, 40];
    cgfx_polyline(pts, 6, 2, 0xFF8800);
    return 0;
}
`);
    // 首参必须经 getelementptr  decay 成 i32*，而不是把 [6 x i32]* 直接传过去
    assert.ok(/getelementptr \[6 x i32\], \[6 x i32\]\* %.+_ptr_\d+, i32 0, i32 0/.test(ir),
              'array decays via GEP to element 0');
    assert.ok(/call void @rvdl_cgfx_polyline\(i32\* %\d+, i32 6, i32 2, i32 16746496\)/.test(ir),
              'call carries the decayed pointer + count/thickness/color');
});

test('ir: cgfx_polyline_a emits the alpha variant', () => {
    const ir = generateIR(`
int32 rvdl_main() {
    arr<int32, 4> pts = [0, 0, 10, 10];
    cgfx_polyline_a(pts, 4, 1, 0x00FF00, 128);
    return 0;
}
`);
    assert.ok(/call void @rvdl_cgfx_polyline_a\(i32\* %\d+, i32 4, i32 1, i32 65280, i32 128\)/.test(ir),
              ir);
});

// ═══════════════════════════════════════════════════════════════════
// nqcmath / expermath 语言层桥接（QMathAbi.hpp）
// ═══════════════════════════════════════════════════════════════════

test('ir: QMathAbi declares the expermath / nqcmath signatures', () => {
    const ir = generateIR('int32 rvdl_main() { return 0; }');
    assert.ok(ir.includes('declare double @rvdl_qstate_purity(%QObject*)'), ir);
    assert.ok(ir.includes('declare double @rvdl_qstate_linear_entropy(%QObject*)'), ir);
    assert.ok(ir.includes('declare double @rvdl_qstate_renyi_entropy(%QObject*, double)'), ir);
    assert.ok(ir.includes('declare double @rvdl_math_dot(double*, double*, i32)'), ir);
    assert.ok(ir.includes('declare double @rvdl_math_norm(double*, i32)'), ir);
});

test('ir: math_dot / math_norm decay double arrays via GEP', () => {
    const ir = generateIR(`
int32 rvdl_main() {
    arr<double, 3> a = [1.0, 2.0, 3.0];
    arr<double, 3> b = [4.0, 5.0, 6.0];
    double d = math_dot(a, b, 3);
    double n = math_norm(a, 3);
    return 0;
}
`);
    assert.ok(/call double @rvdl_math_dot\(double\* %\d+, double\* %\d+, i32 3\)/.test(ir), ir);
    assert.ok(/call double @rvdl_math_norm\(double\* %\d+, i32 3\)/.test(ir), ir);
});

test('ir: qstate_purity on a QObject emits the expermath call', () => {
    const ir = generateIR(`
int32 rvdl_main() {
    QObject q = basis_state(0.0, 0.0, 0);
    double p = qstate_purity(q);
    double le = qstate_linear_entropy(q);
    double re = qstate_renyi_entropy(q, 2.0);
    return 0;
}
`);
    assert.ok(/call double @rvdl_qstate_purity\(%QObject\* %\d+\)/.test(ir), ir);
    assert.ok(/call double @rvdl_qstate_linear_entropy\(%QObject\* %\d+\)/.test(ir), ir);
    assert.ok(/call double @rvdl_qstate_renyi_entropy\(%QObject\* %\d+, double/.test(ir), ir);
});
