import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';
import { IRGenerator } from './ir';

function parse(src: string) {
    return new Parser(new Lexer(src)).parse();
}

function analyze(src: string): SemanticAnalyzer {
    const a = new SemanticAnalyzer();
    a.analyze(parse(src));
    return a;
}

function irOf(src: string): string {
    return new IRGenerator().generate(parse(src));
}

const MAIN = '@layer(time=0, thread=0, coord=(0))\nint32 rvdl_main() { ';

// ─── qrc_new：out_dim 参数化（2 参兼容 / 3 参显式）─────────────────────

test('qrc: qrc_new accepts 2 args (out_dim defaults to 1)', () => {
    const a = analyze(MAIN + 'qrc_new(4, 2); return 0; }');
    assert.ok(!a.errors.some(e => e.message.includes("'qrc_new'")), JSON.stringify(a.errors));
});

test('qrc: qrc_new accepts 3 args with explicit out_dim', () => {
    const a = analyze(MAIN + 'qrc_new(4, 2, 3); return 0; }');
    assert.ok(!a.errors.some(e => e.message.includes("'qrc_new'")), JSON.stringify(a.errors));
});

test('qrc: qrc_new rejects wrong argument count', () => {
    const a1 = analyze(MAIN + 'qrc_new(4); return 0; }');
    assert.ok(a1.errors.some(e => e.message.includes("'qrc_new' expects")),
        'expect signature error for 1 arg');
    const a4 = analyze(MAIN + 'qrc_new(4, 2, 3, 9); return 0; }');
    assert.ok(a4.errors.some(e => e.message.includes("'qrc_new' expects")),
        'expect signature error for 4 args');
});

test('qrc: ir dispatches to rvdl_qrc_new_ex only for the 3-arg form', () => {
    // IR 的 declare 段固定同时包含两个符号，故必须匹配 call 语句才有效。
    const ir2 = irOf(MAIN + 'qrc_new(4, 2); return 0; }');
    assert.ok(/call %QReservoir\* @rvdl_qrc_new\(/.test(ir2), 'expect legacy 2-arg call');

    const ir3 = irOf(MAIN + 'qrc_new(4, 2, 3); return 0; }');
    assert.ok(/call %QReservoir\* @rvdl_qrc_new_ex\(/.test(ir3), 'expect extended call carrying out_dim');
});

// ─── qrc_train_ex：真实训练数据 ABI（8 参 + double 缓冲类型）──────────

test('qrc: qrc_train_ex requires 8 arguments', () => {
    const a = analyze(MAIN + 'qrc_train_ex(1, 2); return 0; }');
    assert.ok(a.errors.some(e => e.message.includes("'qrc_train_ex' expects 8")),
        'expect signature error for wrong arity');
});

test('qrc: qrc_train_ex rejects a non-double buffer for inputs', () => {
    // 第 4 参（inputs）必须是 cap<double> 或 arr<double, N>；此处传入 int32 字面量。
    const a = analyze(MAIN + 'qrc_train_ex(1, 2, 3, 4, 5, 6, 7, 8); return 0; }');
    assert.ok(a.errors.some(e => e.message.includes('must be cap<double> or arr<double, N>')),
        'expect double-buffer type error for inputs');
});

test('qrc: qrc_train_ex declares the 8-arg ABI in the IR preamble', () => {
    const ir = irOf(MAIN + 'qrc_train_ex(1, 2); return 0; }');
    assert.ok(ir.includes('declare void @rvdl_qrc_train_ex(%QReservoir*, i32, double, double*, double*, i32, i32, i32)'),
        'expect rvdl_qrc_train_ex declaration');
});

test('qrc: ir generation survives wrong arity without throwing', () => {
    // 少参调用由语义层报 Signature Error，但 IR 层此前会直接索引
    // arguments[3..7] 抛 TypeError 中断编译 —— 必须对畸形输入保持健壮。
    assert.doesNotThrow(() => irOf(MAIN + 'qrc_train_ex(1, 2); return 0; }'),
        'IR must not throw on malformed qrc_train_ex');
    assert.doesNotThrow(() => irOf(MAIN + 'qrc_new(4); return 0; }'),
        'IR must not throw on malformed qrc_new');
    assert.doesNotThrow(() => irOf(MAIN + 'qrc_new(4, 2); return 0; }'),
        'IR must not throw on 2-arg qrc_new');
});
