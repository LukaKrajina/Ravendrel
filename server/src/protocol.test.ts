import { test } from 'node:test';
import assert from 'node:assert';
import { Cmd, PROTOCOL_VERSION, encodeFrame, encodeResponseFrame, decodeFrames } from './protocol';

test('protocol: encodeFrame produces length-prefixed frame', () => {
    const frame = encodeFrame(Cmd.COMPILE, 'hello IR');
    // 4 字节长度头 + 1 字节命令 + 8 字节 payload
    assert.strictEqual(frame.length, 4 + 1 + 8);
    assert.strictEqual(frame.readUInt32BE(0), 1 + 8);
    assert.strictEqual(frame[4], Cmd.COMPILE);
    assert.strictEqual(frame.subarray(5).toString('utf-8'), 'hello IR');
});

test('protocol: decodeFrames decodes response frames (no command byte)', () => {
    const f1 = encodeResponseFrame('RESPONSE: PONG\n');
    const f2 = encodeResponseFrame('RESPONSE: SUCCESS\n');
    const combined = Buffer.concat([f1, f2]);
    const { frames, rest } = decodeFrames(combined);
    assert.strictEqual(frames.length, 2);
    assert.strictEqual(frames[0], 'RESPONSE: PONG\n');
    assert.strictEqual(frames[1], 'RESPONSE: SUCCESS\n');
    assert.strictEqual(rest.length, 0);
});

test('protocol: decodeFrames handles partial frame', () => {
    const full = encodeResponseFrame('abc');
    const partial = full.subarray(0, 4 + 2); // 截断到长度头 + 部分 payload
    const { frames, rest } = decodeFrames(partial);
    assert.strictEqual(frames.length, 0);
    assert.strictEqual(rest.length, partial.length);
});

test('protocol: HELLO carries version', () => {
    const frame = encodeFrame(Cmd.HELLO, PROTOCOL_VERSION);
    assert.strictEqual(frame[4], Cmd.HELLO);
    assert.strictEqual(frame.subarray(5).toString('utf-8'), PROTOCOL_VERSION);
});

test('protocol: binary payload preserved exactly (no newline truncation)', () => {
    // 关键:含哨兵行(如 END_COMPILE)的 IR 也能完整往返,不会被文本协议截断
    const ir = 'define i32 @f() {\nEND_COMPILE\n  ret i32 0\n}\n';
    const frame = encodeFrame(Cmd.COMPILE, ir);
    // decodeFrames 返回完整 payload = [命令字节][IR]
    const { frames } = decodeFrames(frame);
    assert.strictEqual(frames.length, 1);
    assert.strictEqual(frames[0].charCodeAt(0), Cmd.COMPILE);
    assert.strictEqual(frames[0].substring(1), ir);
});
