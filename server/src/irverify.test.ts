import { test } from 'node:test';
import assert from 'node:assert';
import { verifyIR } from './irverify';

test('irverify: valid IR passes', () => {
    const ir = [
        'define i32 @rvdl_main() {',
        'entry:',
        '  ret i32 0',
        '}'
    ].join('\n');
    assert.deepStrictEqual(verifyIR(ir), []);
});

test('irverify: function missing entry is flagged', () => {
    const ir = [
        'define i32 @foo() {',
        '  ret i32 0',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(d.some(x => x.message.includes("missing an 'entry:'")));
});

test('irverify: unterminated block is flagged', () => {
    const ir = [
        'define i32 @foo() {',
        'entry:',
        '  %1 = add i32 1, 2',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(d.some(x => x.message.includes('not terminated')));
});

test('irverify: register used before definition is flagged', () => {
    const ir = [
        'define i32 @foo() {',
        'entry:',
        '  %2 = add i32 %1, 1',
        '  ret i32 %2',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(d.some(x => x.message.includes("'%1' used before definition")));
});

test('irverify: duplicate SSA definition is flagged', () => {
    const ir = [
        'define i32 @foo() {',
        'entry:',
        '  %1 = add i32 1, 2',
        '  %1 = add i32 3, 4',
        '  ret i32 %1',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(d.some(x => x.message.includes("'%1' defined more than once")));
});

test('irverify: named types and block labels are not mistaken for registers', () => {
    const ir = [
        '%Qubit = type opaque',
        'define i32 @foo() {',
        'entry:',
        '  br label %loop',
        'loop:',
        '  %q = alloca %Qubit*',
        '  br label %loop',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(!d.some(x => x.message.includes("'%Qubit'")), 'type name not flagged');
    assert.ok(!d.some(x => x.message.includes("'%loop'")), 'label not flagged');
});

// ─── SSA 支配关系（真正的支配树，而非「按文本顺序」近似）────────────────

test('irverify: definition not dominating a join-block use is flagged', () => {
    // %x 只在块 A 内定义，但 B 分支直接跳到 C 而未定义 %x，
    // 因此 %x **不支配** C 中的使用 —— 这是真正的 SSA 违规。
    // 「按文本顺序」的旧检查会漏报（%x 的定义文本上确实排在 C 之前）。
    const ir = [
        'define void @foo(i1 %arg0) {',
        'entry:',
        '  br i1 %arg0, label %A, label %B',
        'A:',
        '  %x = add i32 1, 2',
        '  br label %C',
        'B:',
        '  br label %C',
        'C:',
        '  %y = add i32 %x, 1',
        '  ret void',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(
        d.some(x => x.message.includes("'%x'") && x.message.includes('does not dominate')),
        'expect non-dominating definition to be flagged'
    );
});

test('irverify: entry-defined register used across branches is not flagged', () => {
    // %z 在 entry 定义 → 支配所有块，合法，不应误报。
    const ir = [
        'define void @foo(i1 %arg0) {',
        'entry:',
        '  %z = add i32 1, 2',
        '  br i1 %arg0, label %A, label %B',
        'A:',
        '  %a = add i32 %z, 1',
        '  ret void',
        'B:',
        '  %b = add i32 %z, 2',
        '  ret void',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(!d.some(x => x.message.includes('does not dominate')), 'entry-defined register must not be flagged');
});

test('irverify: definition reaching a loop body via back-edge is not flagged', () => {
    // %p 在 entry 定义 → 支配循环体（含自环回边），合法。
    const ir = [
        'define void @foo(i32 %arg0) {',
        'entry:',
        '  %p = add i32 0, 1',
        '  br label %loop',
        'loop:',
        '  %n = add i32 %p, 1',
        '  %c = icmp slt i32 %n, %arg0',
        '  br i1 %c, label %loop, label %done',
        'done:',
        '  ret void',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(!d.some(x => x.message.includes('does not dominate')), 'dominating definition must not be flagged');
});

test('irverify: function parameters are not flagged as non-dominating', () => {
    // %arg0 是函数参数，在入口对所有块可见，跨块使用合法。
    const ir = [
        'define void @foo(i32 %arg0) {',
        'entry:',
        '  %c = icmp slt i32 %arg0, 0',
        '  br i1 %c, label %A, label %B',
        'A:',
        '  %a = add i32 %arg0, 1',
        '  ret void',
        'B:',
        '  %b = add i32 %arg0, 2',
        '  ret void',
        '}'
    ].join('\n');
    const d = verifyIR(ir);
    assert.ok(!d.some(x => x.message.includes('does not dominate')), 'function parameters must not be flagged');
});
