import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';
import { IRGenerator } from './ir';

function parse(src: string) {
    return new Parser(new Lexer(src)).parse();
}

function analyze(src: string) {
    const ast = parse(src);
    const a = new SemanticAnalyzer();
    a.analyze(ast);
    return { ast, errors: a.errors };
}

// ─── flavor 显式赋值 ───────────────────────────────────────────
test('flavor: explicit value assignment', () => {
    const ast = parse('flavor Color { RED = 1, GREEN = 5, BLUE }');
    const flavor = ast.body.find(n => (n as any).type === 'FlavorDecl') as any;
    assert.ok(flavor, 'expect FlavorDecl');
    assert.deepStrictEqual(flavor.members.map((m: any) => m.name), ['RED', 'GREEN', 'BLUE']);
    assert.deepStrictEqual(flavor.members.map((m: any) => m.value), [1, 5, null]);
});

test('flavor: member values inline into IR', () => {
    const { ast, errors } = analyze(
        'flavor Color { RED = 1, GREEN = 5, BLUE }\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { return BLUE; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    // BLUE 自动递增 = 6（GREEN=5 之后 +1）
    assert.ok(ir.includes('ret i32 6'), 'expect BLUE = 6');
});

// ─── 全局变量 / 顶层 fixed 常量 ────────────────────────────────
test('global variable: top-level int32', () => {
    const { ast, errors } = analyze('int32 counter = 0;');
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@counter = global i32 0'), 'expect global counter');
});

test('top-level fixed constant', () => {
    const { ast, errors } = analyze('fixed int32 N = 42;');
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@N = constant i32 42'), 'expect constant N');
});

test('global variable: referenced inside function', () => {
    const { ast, errors } = analyze(
        'int32 counter = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { return counter; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@counter = global i32 0'), 'expect global counter');
    assert.ok(ir.includes('load i32, i32* @counter'), 'expect load @counter');
});

// ─── 函数参数传 cap<T> ─────────────────────────────────────────
test('function param cap<T>', () => {
    const { ast, errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\nint32 f(cap<int32> p) { return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const fn = ast.body.find(n => (n as any).type === 'FunctionDeclaration') as any;
    assert.strictEqual(fn.params[0].type, 'cap<int32>');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('i32* %arg0'), 'expect i32* param for cap<int32>');
});

// ─── 字符串遍历 ────────────────────────────────────────────────
test('string iteration: s[i] + strlen', () => {
    const { ast, errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\n' +
        'int32 f() { string s = "hello"; char c = s[0]; int32 n = strlen(s); return n; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('call i32 @strlen'), 'expect strlen call');
    assert.ok(ir.includes('getelementptr i8, i8*'), 'expect string index gep');
});

// ─── shim.c 三类原语 ───────────────────────────────────────────
test('shim: rvdl_gc_alloc / rvdl_sys_log / quantum gate are declared', () => {
    const { errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\n' +
        'int32 f() { Qubit q = alloc(); h(q); int32 r = measure(q); rvdl_sys_log(0, "hi"); return r; }'
    );
    assert.deepStrictEqual(errors, []);
});

test('shim: kglobals_addr returns global state base', () => {
    const { errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { int32 base = kglobals_addr(); return base; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { int32 base = kglobals_addr(); return base; }'
    ));
    assert.ok(ir.includes('call i32 @kglobals_addr'), 'expect kglobals_addr call');
});

// ─── 全局变量写 / Q-Digest 锁集 ────────────────────────────────
test('global variable: write (reassign)', () => {
    const { ast, errors } = analyze(
        'int32 counter = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { counter = 5; return counter; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('store i32 5, i32* @counter'), 'expect store @counter');
});

test('ir: sync_lock/unlock emit atomic LLVM ops', () => {
    const { ast, errors } = analyze(
        'cap<int32> g_lock = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { sync_lock(g_lock); sync_unlock(g_lock); return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('atomicrmw xchg'), 'expect atomicrmw xchg for sync_lock');
    assert.ok(ir.includes('store atomic'), 'expect store atomic for sync_unlock');
});

test('qdigest: shared lock suppresses race', () => {
    const src = 'cap<int32> g_lock = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 main() { spawn { sync_lock(g_lock); sync_add(g_lock, 1); sync_unlock(g_lock); } spawn { sync_lock(g_lock); sync_add(g_lock, 1); sync_unlock(g_lock); } return 0; }';
    const a = new SemanticAnalyzer(); a.analyze(parse(src));
    assert.ok(!a.errors.some(e => e.message.includes('Race Warning')), 'locked access must not race');
});

test('qdigest: unlocked shared access reports race', () => {
    const src = 'cap<int32> g_lock = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 main() { spawn { sync_add(g_lock, 1); } spawn { sync_add(g_lock, 1); } return 0; }';
    const a = new SemanticAnalyzer(); a.analyze(parse(src));
    assert.ok(a.errors.some(e => e.message.includes('Race Warning')), 'unlocked access must race');
});

test('qdigest: lock propagates across function boundary', () => {
    const src = '@layer(time=0, thread=0, coord=(0))\nvoid critical(cap<int32> l) { sync_lock(l); sync_add(l, 1); sync_unlock(l); }\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 main() { cap<int32> g = 0; spawn { critical(g); } spawn { critical(g); } return 0; }';
    const a = new SemanticAnalyzer(); a.analyze(parse(src));
    assert.ok(!a.errors.some(e => e.message.includes('Race Warning')), 'cross-function lock must propagate');
});

// ─── 锁原语细化：自旋等待 / null 初始化 / align / 取地址 ─────────
test('sync_lock returns old value (spin-wait)', () => {
    const { ast, errors } = analyze(
        'cap<int32> g_lock = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { while (sync_lock(g_lock) != 0) {} sync_unlock(g_lock); return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('atomicrmw xchg'), 'expect atomicrmw xchg');
    assert.ok(ir.includes('align 4'), 'expect align 4');
});

test('sync_lock/sync_unlock emit align 4', () => {
    const { ast } = analyze(
        'cap<int32> g_lock = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { sync_lock(g_lock); sync_unlock(g_lock); return 0; }'
    );
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('atomicrmw xchg'), 'expect atomicrmw xchg');
    assert.ok(ir.includes('store atomic i32 0'), 'expect store atomic');
    assert.ok(ir.includes('align 4'), 'expect align 4');
});

test('global cap<T> init emits null (not 0)', () => {
    const { ast } = analyze('cap<int32> g_buf = 0;');
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@g_buf = global i32* null'), 'expect i32* null');
});

test('address-of global returns cap', () => {
    const { ast, errors } = analyze(
        'int32 counter = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { cap<int32> p = &counter; sync_add(p, 1); return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('@counter'), 'expect @counter address');
});

// ─── native 带操作数 / 16/32 位端口 IO / 上下文切换汇编 ─────────
test('native: supports operands (lidt/fxsave)', () => {
    const { ast, errors } = analyze(
        'cap<int32> buf = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { unsafe { native("lidt (${0})", buf); native("fxsave (${0})", buf); } return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('call void asm sideeffect "lidt (${0})", "r"'), 'expect lidt with operand');
    assert.ok(ir.includes('call void asm sideeffect "fxsave (${0})", "r"'), 'expect fxsave with operand');
});

test('native: zero-operand still works (iretq/hlt)', () => {
    const { errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { unsafe { native("iretq"); } return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { unsafe { native("iretq"); } return 0; }'
    ));
    assert.ok(ir.includes('call void asm sideeffect "iretq"'), 'expect iretq');
});

test('io: outw/inw/outl/inl emit 16/32-bit port asm', () => {
    const { ast, errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { unsafe { outw(0x3F8, 0x41); int32 a = inw(0x3F8); outl(0xCF8, 0x80000000); int32 b = inl(0xCF8); } return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('outw ${0:w}, ${1:w}'), 'expect outw');
    assert.ok(ir.includes('inw ${1:w}, ${0:w}'), 'expect inw');
    assert.ok(ir.includes('outl ${0:k}, ${1:w}'), 'expect outl');
    assert.ok(ir.includes('inl ${1:w}, ${0:k}'), 'expect inl');
});

// ─── 上下文切换汇编（iretq / 切栈 / fxsave）────────────────────
test('context switch: iretq + stack switch + fxsave via native', () => {
    const src =
        '@layer(time=0, thread=0, coord=(0))\n' +
        'void save_fpu(cap<int32> fpu) { unsafe { native("fxsave (${0})", fpu); } }\n' +
        '@layer(time=0, thread=0, coord=(1))\n' +
        'int32 isr() { unsafe { native("iretq"); } return 0; }';
    const ast = parse(src);
    const a = new SemanticAnalyzer(); a.analyze(ast);
    assert.deepStrictEqual(a.errors, []);
    const ir = new IRGenerator().generate(ast);
    assert.ok(ir.includes('fxsave (${0})'), 'expect fxsave');
    assert.ok(ir.includes('iretq'), 'expect iretq');
});

test('context switch: stack switch (mov rsp) via native operand', () => {
    const { errors } = analyze(
        'cap<int32> new_stack = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { unsafe { native("mov %%rsp, ${0}", new_stack); } return 0; }'
    );
    assert.deepStrictEqual(errors, []);
});

// ─── 读寄存器内建（read_cr3 / rdmsr / wrmsr / cpuid）────────────
test('read_cr3 emits mov %cr3 with output operand', () => {
    const { errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { uint64 c = read_cr3(); return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { uint64 c = read_cr3(); return 0; }'
    ));
    assert.ok(ir.includes('call i64 asm sideeffect "mov ${0}, %cr3"'), 'expect read_cr3 asm');
});

test('rdmsr/wrmsr emit EDX:EAX asm', () => {
    const { errors } = analyze(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { uint64 m = rdmsr(0xC0000080); wrmsr(0xC0000080, m); return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { uint64 m = rdmsr(0xC0000080); wrmsr(0xC0000080, m); return 0; }'
    ));
    assert.ok(ir.includes('asm sideeffect "rdmsr"'), 'expect rdmsr');
    assert.ok(ir.includes('asm sideeffect "wrmsr"'), 'expect wrmsr');
});

test('cpuid writes 4 output registers', () => {
    const { errors } = analyze(
        'cap<int32> eax = 0; cap<int32> ebx = 0; cap<int32> ecx = 0; cap<int32> edx = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { cpuid(1, 0, eax, ebx, ecx, edx); return 0; }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(
        'cap<int32> eax = 0; cap<int32> ebx = 0; cap<int32> ecx = 0; cap<int32> edx = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { cpuid(1, 0, eax, ebx, ecx, edx); return 0; }'
    ));
    assert.ok(ir.includes('asm sideeffect "cpuid"'), 'expect cpuid');
    assert.ok(ir.includes('extractvalue { i32, i32, i32, i32 }'), 'expect extractvalue');
});

// ─── 裸汇编块（asm {} + @[naked]）───────────────────────────────
test('asm block + naked emits full assembly body', () => {
    const src =
        '@layer(time=0, thread=0, coord=(0)) @[naked]\n' +
        'void irq_stub() { asm { "push rax; push rbx;" "iretq;" } }\n' +
        '@layer(time=0, thread=0, coord=(1))\nint32 f() { return 0; }';
    const { errors } = analyze(src);
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(src));
    assert.ok(ir.includes('push rax'), 'expect push rax');
    assert.ok(ir.includes('iretq'), 'expect iretq');
    assert.ok(ir.includes(' naked {'), 'expect naked attribute');
    assert.ok(ir.includes('unreachable'), 'expect unreachable terminator');
});

// ─── 函数指针与间接调用（fn<ret(params)>）───────────────────────
test('function pointer + indirect call', () => {
    const src =
        '@layer(time=0, thread=0, coord=(0))\nint32 handler(int32 x) { return x + 1; }\n' +
        '@layer(time=0, thread=0, coord=(1))\nint32 f() { fn<int32(int32)> cb = &handler; return cb(41); }';
    const { errors } = analyze(src);
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(src));
    assert.ok(ir.includes('i32 (i32)*'), 'expect fn pointer type');
    assert.ok(ir.includes('call i32 %'), 'expect indirect call');
});

// ─── @[packed] 精确字节布局 ─────────────────────────────────────
test('@[packed] form emits packed struct (no vtable)', () => {
    const src =
        '@[packed] form GptHeader { uint64 signature; uint32 revision; arr<uint8, 4> reserved; }\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { return 0; }';
    const { errors } = analyze(src);
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(src));
    assert.ok(ir.includes('type <{ i64, i32, [4 x i8] }>'), 'expect packed struct type');
});

// ─── volatile 内存访问 ──────────────────────────────────────────
test('volatile_load/store emit volatile LLVM ops', () => {
    const { errors } = analyze(
        'cap<int32> mmio = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { volatile_store(mmio, 7); return volatile_load(mmio); }'
    );
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(
        'cap<int32> mmio = 0;\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { volatile_store(mmio, 7); return volatile_load(mmio); }'
    ));
    assert.ok(ir.includes('store volatile'), 'expect store volatile');
    assert.ok(ir.includes('load volatile'), 'expect load volatile');
});

// ─── 数组字面量与索引 ───────────────────────────────────────────
test('array literal + index (global + local)', () => {
    const src =
        'fixed arr<int32, 4> SC = [0x1E, 0x30, 0x2E, 0x20];\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { arr<int32, 3> a = [10, 20, 30]; return SC[0] + a[1]; }';
    const { errors } = analyze(src);
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(src));
    assert.ok(ir.includes('@SC = constant [4 x i32]'), 'expect global array constant');
    assert.ok(ir.includes('getelementptr [4 x i32]'), 'expect array gep');
    assert.ok(ir.includes('getelementptr [3 x i32]'), 'expect local array gep');
});

// ─── 完整 uint64 运算（无符号除/取模/比较/右移）─────────────────
test('uint64: unsigned div / rem / compare / lshr', () => {
    const src =
        '@layer(time=0, thread=0, coord=(0))\n' +
        'uint64 lba_to_chs(uint64 lba) { uint64 s = lba / 63; uint64 r = lba % 63; uint64 h = lba >> 4; return s + r + h; }\n' +
        '@layer(time=0, thread=0, coord=(1))\nint32 f() { uint64 a = 0x100000000; int32 c = 0; if (a > 100) { c = 1; } return c; }';
    const { errors } = analyze(src);
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(src));
    assert.ok(ir.includes('udiv'), 'expect udiv for uint64 division');
    assert.ok(ir.includes('urem'), 'expect urem for uint64 modulo');
    assert.ok(ir.includes('lshr'), 'expect lshr for uint64 right shift');
    assert.ok(ir.includes('ugt'), 'expect ugt for uint64 unsigned compare');
});

// ─── 复数类型（complex64 / complex128）──────────────────────────
test('complex128: construct / add / mul / real / cabs / conj', () => {
    const src =
        '@layer(time=0, thread=0, coord=(0))\n' +
        'double f() { complex128 a = complex(1.0, 2.0); complex128 b = complex(3.0, 4.0); complex128 c = a + b; complex128 d = a * b; double r = real(c); double m = cabs(d); complex128 e = conj(a); return r + m; }';
    const { errors } = analyze(src);
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(src));
    assert.ok(ir.includes('{ double, double }'), 'expect complex128 struct type');
    assert.ok(ir.includes('extractvalue'), 'expect extractvalue');
    assert.ok(ir.includes('insertvalue'), 'expect insertvalue');
    assert.ok(ir.includes('llvm.sqrt.f64'), 'expect sqrt for cabs');
});

// ─── 位域（@[packed] 位域打包 + 读改写）────────────────────────
test('bitfield: packed into single storage unit', () => {
    const src =
        '@[packed] form Flags { uint8 a : 1; uint8 b : 1; uint8 c : 6; }\n' +
        '@layer(time=0, thread=0, coord=(0))\nint32 f() { Flags x = new Flags(); x.b = 1; return x.b; }';
    const { errors } = analyze(src);
    assert.deepStrictEqual(errors, []);
    const ir = new IRGenerator().generate(parse(src));
    assert.ok(ir.includes('type <{ i8 }>'), 'expect single i8 storage unit for 3 bitfields');
    assert.ok(ir.includes('lshr'), 'expect lshr for bitfield read');
    assert.ok(ir.includes('shl'), 'expect shl for bitfield write');
    assert.ok(ir.includes(' and '), 'expect and for bitfield mask');
});